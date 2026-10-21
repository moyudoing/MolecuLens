// =============================================================================
// Moleculens · entity · trajectory/xtc/xtc_reader.cpp —— XTC 读取实现
//
// 算法来源：xdrfile（Erik Lindahl & David van der Spoel，BSD-2 许可）的
//   xdrfile_decompress_coord_float / decodebits / decodeints / sizeofints。
//   本文件是按该公开格式**独立重写**的 C++ 版本，并补了两处参考实现没有的防护
//   （见下方 "与参考实现的差异"）。
//
// —— 文件结构（全部 XDR 大端）——
//   每帧：
//     int32   magic = 1995
//     int32   natoms
//     int32   step            （积分步数）
//     float32 time            （ps）
//     float32 box[9]          （nm）
//     int32   lsize           （= natoms，压缩块自带的原子数）
//     —— 以下为 xdrfile_decompress_coord_float 的正文 ——
//     [lsize <= 9]  3×lsize 个 float32 原始坐标（nm，不压缩）
//     [否则]
//       float32 precision     （每 nm 的格点数，GROMACS 通常 1000）
//       int32   minint[3]
//       int32   maxint[3]
//       int32   smallidx      （小整数位宽表的起始下标）
//       int32   nbytes        （后续字节流长度）
//       uint8   payload[nbytes]（XDR opaque，**尾部补齐到 4 字节边界**）
//
// —— 坐标压缩的实质 ——
//   坐标被量化到 precision 网格上成为整数三元组。首原子用 (maxint-minint) 决定的
//   位宽整块编码；随后的原子用「小增量 + 变长位宽」编码，且位宽 smallidx 会随
//   相邻原子差值自适应地上下浮动（水分子被刻意排成 3 个一组，见 run 分支里
//   k==0 的交换）。
//
// —— 与参考实现的差异（本实现有意为之）——
//   1. 位流读取越界即置 overrun 并报错。参考实现的 decodebits 直接按 cnt 索引
//      字节流，损坏/截断文件会读越界内存（UB）。
//   2. run 分支内的原子数受 lsize 约束。参考实现在此不做上界检查，构造出的
//      run 长于实际原子数时会写出缓冲区之外。
//   两处都只在"文件被截断/损坏"时触发；正常 GROMACS 文件的字节流不受影响。
// =============================================================================

#include "entity/trajectory/xtc/xtc_reader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include <glm/glm.hpp>

#include "spdlog/spdlog.h"

namespace moleculens::entity {

namespace {

constexpr int32_t kXtcMagic = 1995;
// GROMACS XTC 内部一律 nm；本工程统一 Å
constexpr float kNmToAngstrom = 10.0f;
// 参考实现里"原子数 ≤ 9 不压缩"的阈值
constexpr int32_t kMinCompressedAtoms = 9;
// magicints 的起始有效下标（magicints[kFirstIdx-1] == 0）
constexpr int kFirstIdx = 9;

// 小整数上界表（与参考实现逐值一致）
const int kMagicInts[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 10, 12, 16, 20, 25, 32, 40, 50, 64,
    80, 101, 128, 161, 203, 256, 322, 406, 512, 645, 812, 1024, 1290,
    1625, 2048, 2580, 3250, 4096, 5060, 6501, 8192, 10321, 13003,
    16384, 20642, 26007, 32768, 41285, 52015, 65536, 82570, 104031,
    131072, 165140, 208063, 262144, 330280, 416127, 524287, 660561,
    832255, 1048576, 1321122, 1664510, 2097152, 2642245, 3329021,
    4194304, 5284491, 6658042, 8388607, 10568983, 13316085, 16777216};
constexpr int kLastIdx = static_cast<int>(sizeof(kMagicInts) / sizeof(kMagicInts[0]));

// XDR 整数/浮点一律大端（xdrfile 经 xdr_ntohl 转换）
int32_t readBE32(const uint8_t* p) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) |
                                static_cast<uint32_t>(p[3]));
}

// float 以 IEEE754 位模式存为大端 int32（xdr_float == xdr_getlong）
float readBEF32(const uint8_t* p) {
    const uint32_t bits = static_cast<uint32_t>(readBE32(p));
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// XDR opaque：数据后补齐到 4 字节边界
size_t roundUp4(size_t n) { return (n + 3u) & ~static_cast<size_t>(3u); }

std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

// —— 位流读取游标 ——
// 对应参考实现里 buf2[0]=已消耗字节数 / buf2[1]=lastbits / buf2[2]=lastbyte
// 三个状态字，payload 从 buf2[3] 起（即字节偏移 12）。
struct BitReader {
    const uint8_t* payload = nullptr;
    size_t size = 0;         // payload 字节数
    size_t cnt = 0;          // 已消耗字节数
    uint32_t lastbits = 0;
    uint32_t lastbyte = 0;
    bool overrun = false;    // 越界读取（截断/损坏文件）—— 参考实现无此检查
};

void readPayloadByte(BitReader& br, uint8_t* out) {
    if (br.cnt >= br.size) {
        br.overrun = true;
        *out = 0;
        return;
    }
    *out = br.payload[br.cnt++];
}

// decodebits：从位流里取 numOfBits 位。位流按"从高到低"打包，lastbits 记录
// 当前字节里还剩多少位未用。
int32_t decodeBits(BitReader& br, int numOfBits) {
    if (numOfBits <= 0) {
        return 0;
    }
    // 参考实现写的是 (1 << numOfBits) - 1；numOfBits 恰为 32 时是 UB，这里显式处理
    const uint32_t mask = numOfBits >= 32 ? 0xFFFFFFFFu : ((1u << numOfBits) - 1u);
    uint32_t num = 0;
    int bits = numOfBits;
    uint8_t c = 0;
    while (bits >= 8) {
        readPayloadByte(br, &c);
        br.lastbyte = (br.lastbyte << 8) | c;
        num |= (br.lastbyte >> br.lastbits) << (bits - 8);
        bits -= 8;
    }
    if (bits > 0) {
        if (br.lastbits < static_cast<uint32_t>(bits)) {
            br.lastbits += 8;
            readPayloadByte(br, &c);
            br.lastbyte = (br.lastbyte << 8) | c;
        }
        br.lastbits -= static_cast<uint32_t>(bits);
        num |= (br.lastbyte >> br.lastbits) & ((1u << bits) - 1u);
    }
    return static_cast<int32_t>(num & mask);
}

// sizeofint：表示 size 所需的最小位数
int sizeofInt(int size) {
    uint32_t num = 1;
    int numOfBits = 0;
    // size 为负时按参考实现的无符号提升语义处理（循环会在 32 位处停下）
    while (static_cast<uint32_t>(size) >= num && numOfBits < 32) {
        ++numOfBits;
        num <<= 1;
    }
    return numOfBits;
}

// sizeofints：numOfInts 个"各自不超 sizes[i]"的整数打包后需要的总位数
int sizeofInts(int numOfInts, const uint32_t sizes[]) {
    uint32_t numOfBytes = 1;
    uint32_t bytes[32] = {};
    bytes[0] = 1;
    uint32_t numOfBits = 0;
    for (int i = 0; i < numOfInts; ++i) {
        uint32_t tmp = 0;
        uint32_t bytecnt = 0;
        for (; bytecnt < numOfBytes; ++bytecnt) {
            tmp = bytes[bytecnt] * sizes[i] + tmp;
            bytes[bytecnt] = tmp & 0xffu;
            tmp >>= 8;
        }
        while (tmp != 0) {
            bytes[bytecnt++] = tmp & 0xffu;
            tmp >>= 8;
        }
        numOfBytes = bytecnt;
    }
    int num = 1;
    --numOfBytes;
    while (bytes[numOfBytes] >= static_cast<uint32_t>(num)) {
        ++numOfBits;
        num *= 2;
    }
    return static_cast<int>(numOfBits + numOfBytes * 8);
}

// decodeints：encodeints 的逆 —— 把一段位流还原成 numOfInts 个"小整数"。
// 做法：先按字节取出全部位，再从高位到低位反复做"除以 sizes[i] 取余"，
// 余数即该位对应的小整数。
void decodeInts(BitReader& br, int numOfInts, int numOfBits, const uint32_t sizes[],
                int32_t nums[]) {
    int bytes[32] = {};
    int numOfBytes = 0;
    while (numOfBits > 8) {
        bytes[numOfBytes++] = decodeBits(br, 8);
        numOfBits -= 8;
    }
    if (numOfBits > 0) {
        bytes[numOfBytes++] = decodeBits(br, numOfBits);
    }
    for (int i = numOfInts - 1; i > 0; --i) {
        int num = 0;
        const int sz = static_cast<int>(sizes[i]);
        for (int j = numOfBytes - 1; j >= 0; --j) {
            num = (num << 8) | bytes[j];
            const int p = num / sz;
            bytes[j] = p;
            num -= p * sz;
        }
        nums[i] = num;
    }
    nums[0] = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
}

// 一帧的元信息（不解码坐标即可得到：用于建帧索引与 dt 推算）
struct FrameMeta {
    uint32_t natoms = 0;
    int32_t  step = 0;
    float    time = 0.0f;
    size_t   totalBytes = 0;   // 本帧整体占用（含 XDR 对齐填充）
};

// 解析帧头并算出整帧长度。失败返回 false 并填 err。
bool parseFrameMeta(const uint8_t* p, size_t avail, FrameMeta* out, std::string* err) {
    // magic/natoms/step/time = 16B，box = 36B，lsize = 4B
    if (avail < 16 + 36 + 4) {
        *err = "XTC 帧头不完整（文件被截断）";
        return false;
    }
    if (readBE32(p) != kXtcMagic) {
        *err = "XTC magic 不匹配（不是 XTC 文件，或字节序/偏移错误）";
        return false;
    }
    out->natoms = static_cast<uint32_t>(readBE32(p + 4));
    out->step = readBE32(p + 8);
    out->time = readBEF32(p + 12);
    if (out->natoms == 0) {
        *err = "XTC 帧原子数为 0";
        return false;
    }

    const size_t afterHeader = 16 + 36;
    const int32_t lsize = readBE32(p + afterHeader);
    if (lsize < 0 || static_cast<uint32_t>(lsize) != out->natoms) {
        *err = "XTC 帧内原子数与帧头不一致（文件损坏）";
        return false;
    }

    size_t body = 0;
    if (lsize <= kMinCompressedAtoms) {
        // 小体系不压缩：3×lsize 个 float32，无 XDR 填充问题（4 字节对齐）
        body = 4 + static_cast<size_t>(lsize) * 3u * 4u;
    } else {
        // precision(4) + minint(12) + maxint(12) + smallidx(4) + nbytes(4) + opaque
        const size_t fixed = 4 + 12 + 12 + 4 + 4;
        if (avail < afterHeader + 4 + fixed) {
            *err = "XTC 压缩块头不完整（文件被截断）";
            return false;
        }
        const int32_t nbytes = readBE32(p + afterHeader + 4 + fixed - 4);
        if (nbytes < 0) {
            *err = "XTC 压缩块长度为负（文件损坏）";
            return false;
        }
        body = 4 + fixed + roundUp4(static_cast<size_t>(nbytes));
    }
    out->totalBytes = afterHeader + body;
    if (out->totalBytes > avail) {
        *err = "XTC 帧数据不完整（文件被截断）";
        return false;
    }
    return true;
}

} // namespace

XtcReader::~XtcReader() { close(); }

bool XtcReader::canOpen(const std::filesystem::path& path) const {
    // 扩展名 + magic 双重判定：仅看扩展名会把改过名的文件误导到 XTC 路径上，
    // 而 XTC 的 magic(1995) 是可靠的（与 DCD 的 "CORD" 同理）
    if (lowerExt(path) != ".xtc") {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    uint8_t head[4] = {};
    in.read(reinterpret_cast<char*>(head), 4);
    return in.gcount() == 4 && readBE32(head) == kXtcMagic;
}

Result<void> XtcReader::open(const std::filesystem::path& path) {
    close();
    // 整文件一次读入。**不要**用 istreambuf_iterator 填充：那是逐字符推进的，
    // MSVC Debug 下带着迭代器调试检查，实测 23 MB 的 md.xtc 要 1.5 s（≈15 MB/s），
    // 而一次 read 是内存带宽级。轨迹文件动辄数十 MB，这个差别直接体现在加载体验上。
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return Result<void>::error(ErrorCode::IoError, "Cannot open " + path.string());
    }
    const std::streamoff fileSize = in.tellg();
    if (fileSize <= 0) {
        return Result<void>::error(ErrorCode::FormatError,
                                   "XTC 文件为空或不可读: " + path.string());
    }
    in.seekg(0, std::ios::beg);
    data_.resize(static_cast<size_t>(fileSize));
    if (!in.read(reinterpret_cast<char*>(data_.data()), fileSize)) {
        data_.clear();
        return Result<void>::error(ErrorCode::IoError, "XTC 读取失败: " + path.string());
    }
    if (data_.size() < 16 + 36 + 4) {
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError,
                                   "XTC 文件过小（不足一帧的帧头）");
    }

    // 索引扫描：只解析帧头 + 压缩块长度，不解码坐标（大文件下这是廉价的一遍）
    std::vector<float> times;
    size_t pos = 0;
    while (pos < data_.size()) {
        // 允许文件尾部的 0 填充（部分写出工具会补齐）
        if (data_.size() - pos < 4) {
            break;
        }
        if (readBE32(data_.data() + pos) != kXtcMagic) {
            break;   // 非帧起点 → 视为文件结束（而非报错：尾部垃圾不该让整份轨迹打不开）
        }
        FrameMeta meta;
        std::string err;
        if (!parseFrameMeta(data_.data() + pos, data_.size() - pos, &meta, &err)) {
            if (frameOffsets_.empty()) {
                // 第一帧就坏 → 格式错误，明确报出
                const std::string msg = path.string() + ": " + err;
                close();
                return Result<void>::error(ErrorCode::FormatError, msg);
            }
            spdlog::warn("[io] XTC 在第 {} 帧处提前结束: {}", frameOffsets_.size(), err);
            break;
        }
        if (frameOffsets_.empty()) {
            atomCount_ = meta.natoms;
        } else if (meta.natoms != atomCount_) {
            // 帧间原子数变化在 XTC 里不合法（拓扑必须一致）→ 截断到最后一致处
            spdlog::warn("[io] XTC 第 {} 帧原子数({}) 与前帧({}) 不一致，忽略其后内容",
                         frameOffsets_.size(), meta.natoms, atomCount_);
            break;
        }
        frameOffsets_.push_back(pos);
        times.push_back(meta.time);
        pos += meta.totalBytes;
    }

    if (frameOffsets_.empty()) {
        close();
        return Result<void>::error(ErrorCode::FormatError, "XTC 不含任何有效帧");
    }
    frameCount_ = frameOffsets_.size();

    // dt：XTC 每帧自带时间（ps）。用首末帧差 / 间隔数得到平均帧间隔 ——
    // 比单看前两帧稳健（首帧时间偶有非零起点）。
    dt_ = 1.0f;
    if (times.size() >= 2) {
        const float span = times.back() - times.front();
        if (span > 0.0f) {
            dt_ = span / static_cast<float>(times.size() - 1);
        }
    }

    nextIndex_ = 0;
    open_ = true;
    spdlog::info("[io] XTC 已打开: {} ({} 原子 × {} 帧, dt={} ps)", path.filename().string(),
                 atomCount_, frameCount_, dt_);
    return Result<void>::ok();
}

Result<Timestep> XtcReader::readFrame(uint64_t index) {
    if (!open_ || index >= frameOffsets_.size()) {
        return Result<Timestep>::error(ErrorCode::InvalidState,
                                       "XTC 帧下标越界或读取器未打开");
    }
    const uint8_t* frame = data_.data() + frameOffsets_[static_cast<size_t>(index)];
    const size_t avail = data_.size() - frameOffsets_[static_cast<size_t>(index)];

    FrameMeta meta;
    std::string err;
    if (!parseFrameMeta(frame, avail, &meta, &err)) {
        return Result<Timestep>::error(ErrorCode::ParseError, err);
    }

    Timestep ts;
    ts.positions.resize(meta.natoms);
    const size_t afterHeader = 16 + 36;
    const int32_t lsize = readBE32(frame + afterHeader);
    const size_t bodyBase = afterHeader + 4;   // 压缩块正文起点（跳过 lsize）

    if (lsize <= kMinCompressedAtoms) {
        // 未压缩：直接 3×lsize 个 float32，单位 nm
        if (avail < bodyBase + static_cast<size_t>(lsize) * 12u) {
            return Result<Timestep>::error(ErrorCode::ParseError, "XTC 原始坐标块被截断");
        }
        for (int32_t i = 0; i < lsize; ++i) {
            const uint8_t* c = frame + bodyBase + static_cast<size_t>(i) * 12u;
            ts.positions[static_cast<size_t>(i)] =
                glm::vec3(readBEF32(c), readBEF32(c + 4), readBEF32(c + 8)) *
                kNmToAngstrom;
        }
    } else {
        const uint8_t* p = frame + bodyBase;
        // 压缩块：precision / minint / maxint / smallidx / nbytes / payload
        const float precision = readBEF32(p);
        if (!(precision > 0.0f)) {
            return Result<Timestep>::error(ErrorCode::ParseError,
                                           "XTC 压缩精度非法（<=0），文件可能损坏");
        }
        const int32_t minint[3] = {readBE32(p + 4), readBE32(p + 8), readBE32(p + 12)};
        const int32_t maxint[3] = {readBE32(p + 16), readBE32(p + 20), readBE32(p + 24)};
        const int32_t smallidx = readBE32(p + 28);
        const int32_t nbytes = readBE32(p + 32);
        const uint8_t* payload = p + 36;

        if (smallidx < kFirstIdx || smallidx >= kLastIdx) {
            return Result<Timestep>::error(ErrorCode::ParseError,
                                           "XTC smallidx 越界，文件可能损坏");
        }
        if (nbytes < 0 || avail < bodyBase + 36u + static_cast<size_t>(nbytes)) {
            return Result<Timestep>::error(ErrorCode::ParseError, "XTC 压缩数据被截断");
        }

        // 每维的取值跨度 → 首原子的整块位宽
        uint32_t sizeint[3];
        for (int d = 0; d < 3; ++d) {
            sizeint[d] = static_cast<uint32_t>(maxint[d] - minint[d] + 1);
        }
        int bitsizeint[3] = {0, 0, 0};
        uint32_t bitsize = 0;
        // 参考实现：跨度大于 0xffffff 时改用逐维定长位宽（避免乘法溢出）
        if ((sizeint[0] | sizeint[1] | sizeint[2]) > 0xffffffu) {
            bitsizeint[0] = sizeofInt(static_cast<int>(sizeint[0]));
            bitsizeint[1] = sizeofInt(static_cast<int>(sizeint[1]));
            bitsizeint[2] = sizeofInt(static_cast<int>(sizeint[2]));
        } else {
            bitsize = static_cast<uint32_t>(sizeofInts(3, sizeint));
        }

        // 小整数自适应位宽：smallidx 随相邻原子差值的实际大小上下浮动
        int idx = smallidx;
        int smallerIdx = idx - 1;
        smallerIdx = kFirstIdx > smallerIdx ? kFirstIdx : smallerIdx;
        int smaller = kMagicInts[smallerIdx] / 2;
        int smallnum = kMagicInts[idx] / 2;
        uint32_t sizesmall[3] = {static_cast<uint32_t>(kMagicInts[idx]),
                                 static_cast<uint32_t>(kMagicInts[idx]),
                                 static_cast<uint32_t>(kMagicInts[idx])};

        BitReader br;
        br.payload = payload;
        br.size = static_cast<size_t>(nbytes);

        const float invPrecision = 1.0f / precision;
        int run = 0;
        uint32_t i = 0;
        int32_t prevcoord[3] = {0, 0, 0};

        while (i < meta.natoms) {
            int32_t coord[3] = {0, 0, 0};
            if (bitsize == 0) {
                coord[0] = decodeBits(br, bitsizeint[0]);
                coord[1] = decodeBits(br, bitsizeint[1]);
                coord[2] = decodeBits(br, bitsizeint[2]);
            } else {
                decodeInts(br, 3, static_cast<int>(bitsize), sizeint, coord);
            }
            ++i;
            for (int d = 0; d < 3; ++d) {
                coord[d] += minint[d];
            }
            for (int d = 0; d < 3; ++d) {
                prevcoord[d] = coord[d];
            }

            const int32_t flag = decodeBits(br, 1);
            int isSmaller = 0;
            if (flag == 1) {
                run = decodeBits(br, 5);
                isSmaller = run % 3;
                run -= isSmaller;
                --isSmaller;
            }
            if (run > 0) {
                // 水上优化：连续 3 个原子按同一小位宽编码；k==0 的那组要与前一个
                // 原子交换 —— 于是输出顺序变成 (本组第 1 个, 前一个原子, 本组余下)
                for (int k = 0; k < run; k += 3) {
                    if (i >= meta.natoms) {
                        // 参考实现无此检查（会越界写）；截断文件在此收敛为短帧
                        break;
                    }
                    int32_t c[3] = {0, 0, 0};
                    decodeInts(br, 3, idx, sizesmall, c);
                    ++i;
                    for (int d = 0; d < 3; ++d) {
                        c[d] += prevcoord[d] - smallnum;
                    }
                    if (k == 0) {
                        for (int d = 0; d < 3; ++d) {
                            std::swap(c[d], prevcoord[d]);
                        }
                        const size_t atom = i - 1;
                        ts.positions[atom - 1] =   // 上一个原子（交换后落在 prevcoord）
                            glm::vec3(static_cast<float>(prevcoord[0]),
                                      static_cast<float>(prevcoord[1]),
                                      static_cast<float>(prevcoord[2])) *
                            invPrecision * kNmToAngstrom;
                        ts.positions[atom] =
                            glm::vec3(static_cast<float>(c[0]), static_cast<float>(c[1]),
                                      static_cast<float>(c[2])) *
                            invPrecision * kNmToAngstrom;
                    } else {
                        for (int d = 0; d < 3; ++d) {
                            prevcoord[d] = c[d];
                        }
                        ts.positions[i - 1] =
                            glm::vec3(static_cast<float>(c[0]), static_cast<float>(c[1]),
                                      static_cast<float>(c[2])) *
                            invPrecision * kNmToAngstrom;
                    }
                }
            } else {
                ts.positions[i - 1] =
                    glm::vec3(static_cast<float>(coord[0]), static_cast<float>(coord[1]),
                              static_cast<float>(coord[2])) *
                    invPrecision * kNmToAngstrom;
            }

            // 位宽自适应（与参考实现逐步对应）
            idx += isSmaller;
            if (isSmaller < 0) {
                smallnum = smaller;
                smaller = idx > kFirstIdx ? kMagicInts[idx - 1] / 2 : 0;
            } else if (isSmaller > 0) {
                smaller = smallnum;
                smallnum = kMagicInts[idx] / 2;
            }
            if (idx < kFirstIdx || idx >= kLastIdx) {
                return Result<Timestep>::error(ErrorCode::ParseError,
                                               "XTC 位宽下标越界，压缩数据损坏");
            }
            sizesmall[0] = sizesmall[1] = sizesmall[2] =
                static_cast<uint32_t>(kMagicInts[idx]);
        }

        if (br.overrun) {
            return Result<Timestep>::error(ErrorCode::ParseError,
                                           "XTC 位流越界（压缩数据损坏或被截断）");
        }
    }

    // 盒子（nm → Å）
    for (int k = 0; k < 9; ++k) {
        ts.box[static_cast<size_t>(k)] = readBEF32(frame + 16 + k * 4) * kNmToAngstrom;
    }
    return Result<Timestep>::ok(std::move(ts));
}

Result<Timestep> XtcReader::nextFrame() {
    if (!open_ || nextIndex_ >= frameCount_) {
        return Result<Timestep>::error(ErrorCode::InvalidState, "XTC 已到末帧");
    }
    return readFrame(nextIndex_++);
}

void XtcReader::close() {
    data_.clear();
    frameOffsets_.clear();
    atomCount_ = 0;
    frameCount_ = 0;
    nextIndex_ = 0;
    dt_ = 1.0f;
    open_ = false;
}

} // namespace moleculens::entity
