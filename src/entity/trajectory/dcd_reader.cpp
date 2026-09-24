// =============================================================================
// MolecuLens · entity · trajectory/dcd_reader.cpp —— DCD 读取实现
// =============================================================================

#include "entity/trajectory/dcd_reader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string>

#include <glm/glm.hpp>

#include "spdlog/spdlog.h"

namespace moleculens::entity {

namespace {

// DCD 所有整数字段均为 int32（本实现按 little-endian；DCD 的字节序由文件头
// 首字段是否为 84 判定，大小端混用极罕见）
int32_t readI32(const uint8_t* p) {
    int32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

float readF32(const uint8_t* p) {
    float v = 0.0f;
    std::memcpy(&v, p, 4);
    return v;
}

std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

} // namespace

DcdReader::~DcdReader() { close(); }

bool DcdReader::canOpen(const std::filesystem::path& path) const {
    return lowerExt(path) == ".dcd";
}

Result<void> DcdReader::open(const std::filesystem::path& path) {
    close();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<void>::error(ErrorCode::IoError, "Cannot open " + path.string());
    }
    data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (data_.size() < 92) {
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError, "DCD too small");
    }

    // 固定头：记录长度 84 + 标识 "CORD"
    if (readI32(data_.data()) != 84 || std::memcmp(data_.data() + 4, "CORD", 4) != 0) {
        if (std::memcmp(data_.data() + 4, "VELD", 4) == 0) {
            data_.clear();
            return Result<void>::error(ErrorCode::FormatError,
                                       "DCD 为速度文件（VELD），当前仅支持坐标");
        }
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError, "not a DCD file (bad magic)");
    }

    const int32_t nset = readI32(data_.data() + 8);
    const int32_t nsavc = readI32(data_.data() + 16);

    // 标题记录：len + 文本 + len
    size_t pos = 84;
    if (pos + 4 > data_.size()) {
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError, "truncated title record");
    }
    const int32_t titleLen = readI32(data_.data() + pos);
    if (titleLen < 0 || pos + 4 + static_cast<size_t>(titleLen) + 4 > data_.size()) {
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError, "bad title record");
    }
    pos += 4 + static_cast<size_t>(titleLen) + 4;

    // 原子数记录：len(4) + natom(4) + len(4)
    if (pos + 12 > data_.size() || readI32(data_.data() + pos) != 4) {
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError, "bad atom-count record");
    }
    const int32_t natom = readI32(data_.data() + pos + 4);
    if (natom <= 0) {
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError, "invalid atom count");
    }
    atomCount_ = static_cast<uint32_t>(natom);
    pos += 12;

    // 可选 DELTA（时间步长，ps）：len(8) + double + len(8)
    float delta = 0.0f;
    if (pos + 16 <= data_.size() && readI32(data_.data() + pos) == 8) {
        double d = 0.0;
        std::memcpy(&d, data_.data() + pos + 4, 8);
        delta = static_cast<float>(d);
        pos += 16;
    }

    framesBegin_ = pos;
    // 每帧 3 个分量，各为 len(4) + natom*4 + len(4)
    frameStride_ = 3u * (8u + static_cast<size_t>(natom) * 4u);
    if (frameStride_ == 0) {
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError, "invalid frame stride");
    }

    // 帧数以头部 NSET 为准，但按实际文件长度收敛（NSET 偶有不可信）
    const size_t avail = (data_.size() > framesBegin_) ? data_.size() - framesBegin_ : 0;
    const uint64_t fitFrames = static_cast<uint64_t>(avail / frameStride_);
    uint64_t declared = (nset > 0) ? static_cast<uint64_t>(nset) : fitFrames;
    if (declared > fitFrames) {
        spdlog::warn("[dcd] 头部声明 {} 帧，文件实际只容纳 {} 帧 —— 按后者读取",
                     declared, fitFrames);
        declared = fitFrames;
    }
    frameCount_ = declared;
    if (frameCount_ == 0) {
        data_.clear();
        return Result<void>::error(ErrorCode::FormatError, "DCD 不含完整帧");
    }

    // 帧间隔：NSAVC × DELTA（缺 DELTA 时按 1.0）
    dt_ = (nsavc > 0) ? (static_cast<float>(nsavc) * (delta > 0.0f ? delta : 1.0f))
                      : (delta > 0.0f ? delta : 1.0f);
    nextIndex_ = 0;
    open_ = true;
    spdlog::info("[dcd] opened {}: {} 原子 × {} 帧, dt={} ps", path.string(), atomCount_,
                 frameCount_, dt_);
    return Result<void>::ok();
}

Result<Timestep> DcdReader::readFrame(uint64_t index) {
    if (!open_) {
        return Result<Timestep>::error(ErrorCode::InvalidState, "DCD not open");
    }
    if (index >= frameCount_) {
        return Result<Timestep>::error(ErrorCode::NotFound, "frame index out of range");
    }
    const size_t base = framesBegin_ + static_cast<size_t>(index) * frameStride_;
    Timestep ts;
    ts.positions.resize(atomCount_);

    size_t p = base;
    for (int axis = 0; axis < 3; ++axis) {
        if (p + 4 > data_.size()) {
            return Result<Timestep>::error(ErrorCode::FormatError, "truncated frame");
        }
        const int32_t len = readI32(data_.data() + p);
        p += 4;
        const size_t bytes = static_cast<size_t>(atomCount_) * 4u;
        if (len < 0 || static_cast<size_t>(len) != bytes || p + bytes > data_.size()) {
            return Result<Timestep>::error(ErrorCode::FormatError,
                                           "frame component length mismatch");
        }
        for (uint32_t i = 0; i < atomCount_; ++i) {
            ts.positions[i][axis] = readF32(data_.data() + p + static_cast<size_t>(i) * 4u);
        }
        p += bytes + 4;   // 跳过尾部长度标记
    }
    return Result<Timestep>::ok(std::move(ts));
}

Result<Timestep> DcdReader::nextFrame() {
    if (nextIndex_ >= frameCount_) {
        return Result<Timestep>::error(ErrorCode::NotFound, "no more frames");
    }
    auto r = readFrame(nextIndex_);
    if (r) {
        ++nextIndex_;
    }
    return r;
}

void DcdReader::close() {
    data_.clear();
    data_.shrink_to_fit();
    framesBegin_ = 0;
    frameStride_ = 0;
    atomCount_ = 0;
    frameCount_ = 0;
    nextIndex_ = 0;
    dt_ = 1.0f;
    open_ = false;
}

} // namespace moleculens::entity
