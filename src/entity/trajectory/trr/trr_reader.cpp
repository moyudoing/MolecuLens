// =============================================================================
// MolecuLens · entity · trajectory/trr/trr_reader.cpp —— TRR 读取实现
//
// 算法：TRR 是无压缩的 XDR 记录流。与 XTC 不同，这里不需要位流解码：
//   1) 整文件读入；
//   2) 顺序扫描每帧 64 字节头，按 box/vir/pres/x/v/f 的**字节长度**跳过负载，
//      记录帧偏移；任一帧头不合法（magic/长度越界）即截断；
//   3) readFrame 按偏移取坐标：x_size 为 natoms*3*4 → float，natoms*3*8 → double。
//
// 防御（与参考实现 xdrfile 的差异）：
//   · 所有偏移/长度先与文件大小比较，截断文件报 ParseError 而不是越界读；
//   · 帧间原子数必须一致，否则拒绝（轨迹按体系总原子数挂载，帧间变长会让
//     播放错位）；
//   · box_size 只接受 36（float）或 72（double），其它值视为损坏。
// =============================================================================

#include "entity/trajectory/trr/trr_reader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include <glm/glm.hpp>

#include "spdlog/spdlog.h"

namespace moleculens::entity {

namespace {

constexpr int32_t kTrrMagic = 1993;
// 经典布局（version 1/2）：14 × int32 + 2 × float32，负载紧跟其后
constexpr size_t kLegacyHeaderBytes = 64;
// 新布局（GROMACS 2023+，version 13）：magic + version + hdr_size + "GMX_trn_file"
// （12 字节标记）+ 13 × int32 + 2 × float32
constexpr size_t kModernHeaderBytes = 84;
// GROMACS TRR 内部一律 nm；本工程统一 Å
constexpr float kNmToAngstrom = 10.0f;

int32_t readBE32(const uint8_t* p) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) |
                                static_cast<uint32_t>(p[3]));
}

float readBEF32(const uint8_t* p) {
    const uint32_t bits = static_cast<uint32_t>(readBE32(p));
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

double readBED64(const uint8_t* p) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = (bits << 8) | static_cast<uint64_t>(p[i]);
    }
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

struct TrrHeader {
    int32_t magic = 0;
    int32_t version = 0;
    int32_t ir = 0, e = 0, box = 0, vir = 0, pres = 0, top = 0;
    int32_t x = 0, v = 0, f = 0;
    int32_t natoms = 0;
    int32_t step = 0;
    int32_t nre = 0;
    float   time = 0.0f;
    float   lambda = 0.0f;
    size_t  headerBytes = kLegacyHeaderBytes;   // 头长度（经典 64 / 新 84）

    [[nodiscard]] size_t payloadBytes() const {
        return static_cast<size_t>(box) + static_cast<size_t>(vir) +
               static_cast<size_t>(pres) + static_cast<size_t>(x) +
               static_cast<size_t>(v) + static_cast<size_t>(f);
    }
};

// 解析 off 处的帧头；越界/魔数不符返回 false。
// 兼容两种布局：
//   经典（version 1/2）：magic, version, ir, e, box, vir, pres, top, x, v, f,
//                        natoms, step, nre, t, lambda —— 64 字节
//   新式（version 13，GROMACS 2023+）：magic, version, hdr_size, "GMX_trn_file",
//                        13 × int32（多一个保留字段），t, lambda —— 84 字节
bool parseHeader(const std::vector<uint8_t>& data, size_t off, TrrHeader* out) {
    constexpr size_t kMinHeader = 64;
    if (off + kMinHeader > data.size()) {
        return false;
    }
    const uint8_t* p = data.data() + off;
    out->magic = readBE32(p + 0);
    if (out->magic != kTrrMagic) {
        return false;
    }
    out->version = readBE32(p + 4);
    if (out->version < 1 || out->version > 20) {
        return false;
    }

    if (out->version >= 13) {
        if (off + kModernHeaderBytes > data.size()) {
            return false;
        }
        // "GMX_trn_file" 标记（12 字节，紧跟 hdr_size 字段）
        static const char kMarker[12] = {'G', 'M', 'X', '_', 't', 'r', 'n',
                                         '_', 'f', 'i', 'l', 'e'};
        if (std::memcmp(p + 12, kMarker, sizeof(kMarker)) != 0) {
            return false;
        }
        const uint8_t* b = p + 24;   // 之后 13 × int32 + 2 × float32
        out->ir = readBE32(b + 0);
        out->e = readBE32(b + 4);
        out->box = readBE32(b + 8);
        out->vir = readBE32(b + 12);
        out->pres = readBE32(b + 16);
        out->top = readBE32(b + 20);
        /* b + 24：保留位（实测为 0） */
        out->x = readBE32(b + 28);
        out->v = readBE32(b + 32);
        out->f = readBE32(b + 36);
        out->natoms = readBE32(b + 40);
        out->step = readBE32(b + 44);
        out->nre = readBE32(b + 48);
        out->time = readBEF32(b + 52);
        out->lambda = readBEF32(b + 56);
        out->headerBytes = kModernHeaderBytes;
    } else {
        out->ir = readBE32(p + 8);
        out->e = readBE32(p + 12);
        out->box = readBE32(p + 16);
        out->vir = readBE32(p + 20);
        out->pres = readBE32(p + 24);
        out->top = readBE32(p + 28);
        out->x = readBE32(p + 32);
        out->v = readBE32(p + 36);
        out->f = readBE32(p + 40);
        out->natoms = readBE32(p + 44);
        out->step = readBE32(p + 48);
        out->nre = readBE32(p + 52);
        out->time = readBEF32(p + 56);
        out->lambda = readBEF32(p + 60);
        out->headerBytes = kLegacyHeaderBytes;
    }

    if (out->natoms <= 0) {
        return false;
    }
    // 字段长度必须是 4/8 字节的整数倍，且非负
    const int32_t sizes[6] = {out->box, out->vir, out->pres, out->x, out->v, out->f};
    for (const int32_t s : sizes) {
        if (s < 0 || (s % 4) != 0) {
            return false;
        }
    }
    if (out->box != 36 && out->box != 72) {
        return false;
    }
    const int64_t atoms3 = static_cast<int64_t>(out->natoms) * 3;
    if (out->x != atoms3 * 4 && out->x != atoms3 * 8) {
        return false;
    }
    if (off + out->headerBytes + out->payloadBytes() > data.size()) {
        return false;
    }
    return true;
}

// 按 size 判定字段宽度：natoms*3*4 → float，natoms*3*8 → double
template <typename Fn>
void forEachVec3(const uint8_t* base, int32_t byteSize, int32_t natoms, Fn&& fn) {
    const bool dbl = byteSize == static_cast<int32_t>(natoms) * 3 * 8;
    for (int32_t i = 0; i < natoms; ++i) {
        glm::vec3 v;
        if (dbl) {
            v.x = static_cast<float>(readBED64(base + static_cast<size_t>(i) * 24 + 0));
            v.y = static_cast<float>(readBED64(base + static_cast<size_t>(i) * 24 + 8));
            v.z = static_cast<float>(readBED64(base + static_cast<size_t>(i) * 24 + 16));
        } else {
            v.x = readBEF32(base + static_cast<size_t>(i) * 12 + 0);
            v.y = readBEF32(base + static_cast<size_t>(i) * 12 + 4);
            v.z = readBEF32(base + static_cast<size_t>(i) * 12 + 8);
        }
        fn(i, v);
    }
}

} // namespace

TrrReader::~TrrReader() = default;

bool TrrReader::canOpen(const std::filesystem::path& path) const {
    if (lowerExt(path) != ".trr") {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    uint8_t magic[4] = {};
    in.read(reinterpret_cast<char*>(magic), 4);
    return in.gcount() == 4 && readBE32(magic) == kTrrMagic;
}

Result<void> TrrReader::open(const std::filesystem::path& path) {
    close();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<void>::error(ErrorCode::NotFound,
                                   "TRR 文件不存在或无法打开: " + path.string());
    }
    data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (data_.size() < kLegacyHeaderBytes) {
        return Result<void>::error(ErrorCode::ParseError, "TRR 文件过短，缺少帧头");
    }

    // 扫描帧偏移：逐帧按头里的字节长度跳转
    size_t off = 0;
    TrrHeader first;
    while (parseHeader(data_, off, &first)) {
        if (frameOffsets_.empty()) {
            atomCount_ = static_cast<uint32_t>(first.natoms);
        } else if (static_cast<uint32_t>(first.natoms) != atomCount_) {
            spdlog::warn("[trr] 第 {} 帧原子数 {} 与首帧 {} 不一致，已截断", frameOffsets_.size(),
                         first.natoms, atomCount_);
            break;
        }
        frameOffsets_.push_back(off);
        off += first.headerBytes + first.payloadBytes();
    }

    if (frameOffsets_.empty()) {
        data_.clear();
        return Result<void>::error(
            ErrorCode::FormatError,
            "不是有效的 TRR 文件（magic 应为 1993，或首帧头损坏）: " + path.string());
    }

    frameCount_ = frameOffsets_.size();
    nextIndex_ = 0;

    // dt：优先用前两帧时间差；只有一帧时退回 1.0 ps
    dt_ = 1.0f;
    if (frameCount_ >= 2) {
        TrrHeader h0, h1;
        parseHeader(data_, frameOffsets_[0], &h0);
        parseHeader(data_, frameOffsets_[1], &h1);
        const float d = h1.time - h0.time;
        if (d > 1e-6f) {
            dt_ = d;
        }
    }

    open_ = true;
    return Result<void>::ok();
}

Result<Timestep> TrrReader::readFrame(uint64_t index) {
    if (!open_ || index >= frameCount_) {
        return Result<Timestep>::error(ErrorCode::InvalidState, "TRR 帧索引越界");
    }
    TrrHeader h;
    if (!parseHeader(data_, frameOffsets_[index], &h)) {
        return Result<Timestep>::error(ErrorCode::ParseError, "TRR 帧头损坏");
    }
    Timestep ts;
    ts.positions.resize(static_cast<size_t>(h.natoms));

    const uint8_t* base = data_.data() + frameOffsets_[index] + h.headerBytes;
    // box：9 × float/double（nm → Å）
    if (h.box == 36) {
        for (int k = 0; k < 9; ++k) {
            ts.box[static_cast<size_t>(k)] = readBEF32(base + k * 4) * kNmToAngstrom;
        }
    } else {
        for (int k = 0; k < 9; ++k) {
            ts.box[static_cast<size_t>(k)] =
                static_cast<float>(readBED64(base + k * 8)) * kNmToAngstrom;
        }
    }
    size_t p = static_cast<size_t>(h.box) + static_cast<size_t>(h.vir) +
               static_cast<size_t>(h.pres);

    // 坐标（nm → Å）
    forEachVec3(base + p, h.x, h.natoms, [&](int32_t i, const glm::vec3& v) {
        ts.positions[static_cast<size_t>(i)] = v * kNmToAngstrom;
    });
    p += static_cast<size_t>(h.x);

    // 速度（可选；nm/ps → Å/ps）
    if (h.v > 0 && h.v == h.x) {
        ts.velocities.resize(static_cast<size_t>(h.natoms));
        forEachVec3(base + p, h.v, h.natoms, [&](int32_t i, const glm::vec3& v) {
            ts.velocities[static_cast<size_t>(i)] = v * kNmToAngstrom;
        });
    }
    return Result<Timestep>::ok(std::move(ts));
}

Result<Timestep> TrrReader::nextFrame() {
    if (!open_ || nextIndex_ >= frameCount_) {
        return Result<Timestep>::error(ErrorCode::InvalidState, "TRR 已到末帧");
    }
    return readFrame(nextIndex_++);
}

void TrrReader::close() {
    data_.clear();
    data_.shrink_to_fit();
    frameOffsets_.clear();
    atomCount_ = 0;
    frameCount_ = 0;
    nextIndex_ = 0;
    dt_ = 1.0f;
    open_ = false;
}

} // namespace moleculens::entity
