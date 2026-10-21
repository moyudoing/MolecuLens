// =============================================================================
// MolecuLens · engine · cmd/command_list.cpp —— SceneCommandList 实现
// 规格来源：主方案 §5.3 / 附录 F（16B 对齐内存池；字符串池；serial；
//           迭代器；序列化 writeTo/readFrom 字节布局）
// =============================================================================

#include "engine/cmd/cmd.h"

#include <cstring>

#include "spdlog/spdlog.h"

namespace moleculens::engine {

// —— CommandIterator 实现（SceneCommandList 完整定义之后）——
CommandIterator::CommandIterator(const SceneCommandList& list) : list_(&list) {
    readHeader();
}

bool CommandIterator::done() const { return done_; }
Cmd CommandIterator::code() const { return header_.code; }
const uint8_t* CommandIterator::payload() const { return payload_; }
uint32_t CommandIterator::payloadSize() const { return header_.size; }

void CommandIterator::next() {
    if (done_) {
        return;
    }
    // 前进到下一命令（含 16B 对齐）
    const uint32_t total = sizeof(CmdHeader) + header_.size;
    pos_ += (total + kCmdAlign - 1) & ~(kCmdAlign - 1);
    readHeader();
}

void CommandIterator::readHeader() {
    if (!list_ || pos_ + sizeof(CmdHeader) > list_->pool_.size()) {
        done_ = true;
        return;
    }
    std::memcpy(&header_, list_->pool_.data() + pos_, sizeof(CmdHeader));
    payload_ = list_->pool_.data() + pos_ + sizeof(CmdHeader);
    if (header_.size > kCmdMaxPayload ||
        pos_ + sizeof(CmdHeader) + header_.size > list_->pool_.size()) {
        // ⚠ 这里是**静默截断**：done_ 一置，其后所有命令（包括几何）全部不可见，
        // 而命令池里明明有数据 —— 表现为"算出来了但画不出来"，且没有任何报错。
        // 历史上整体系表面就是撞在这条上（单块 19–36 MB > 16 MiB 上限）。
        // 现在发射端按上限切块（appendTriMesh），正常路径不该再走到这里；
        // 真走到这里说明载荷超限或池被写坏 —— 必须**出声**，不要再静默。
        spdlog::error("[cmd] 命令流解析中止：载荷 {} 字节超上限 {}（或越过池尾 {}）—— "
                      "其后命令全部不可见。请检查发射端是否按 kCmdMaxPayload 切块。",
                      header_.size, kCmdMaxPayload, list_->pool_.size());
        done_ = true;
        return;
    }
    done_ = false;
}

void SceneCommandList::append(Cmd code, const void* payload, uint32_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(payload);

    // —— 重放模式：把载荷写回记录的偏移，不增长池、不重写命令头 ——
    // 命令序列（含 16B 对齐填充与头）与全量重建时逐字节相同，故只覆盖载荷。
    if (replaying_) {
        if (replayCursor_ >= replay_.size()) {
            replayFailed_ = true;   // 发射了计划外的命令
            return;
        }
        const ReplayEntry& e = replay_[replayCursor_];
        if (e.code != code || e.size != bytes ||
            static_cast<size_t>(e.offset) + bytes > pool_.size()) {
            replayFailed_ = true;
            return;
        }
        std::memcpy(pool_.data() + e.offset, p, bytes);
        ++replayCursor_;
        return;
    }

    // 16B 对齐填充（前一条命令尾部）
    while (pool_.size() % kCmdAlign != 0) {
        pool_.push_back(0);
    }
    CmdHeader header;
    header.code = code;
    header.pad[0] = header.pad[1] = header.pad[2] = 0;
    header.size = bytes;
    const uint32_t payloadOffset =
        static_cast<uint32_t>(pool_.size()) + static_cast<uint32_t>(sizeof(CmdHeader));
    pool_.insert(pool_.end(), reinterpret_cast<const uint8_t*>(&header),
                 reinterpret_cast<const uint8_t*>(&header) + sizeof(CmdHeader));
    pool_.insert(pool_.end(), p, p + bytes);

    if (code == Cmd::TriMesh) {
        hasMesh_ = true;
    }
    if (recording_ && isCoordCommand(code)) {
        replay_.push_back(ReplayEntry{payloadOffset, bytes, code});
    }
}

void SceneCommandList::appendTriMesh(const std::vector<TriMeshVertex>& verts,
                                     const std::vector<uint32_t>& indices) {
    if (verts.empty() || indices.size() < 3) {
        return;
    }
    // 每块的字节预算：留出命令头 + 16B 对齐的余量
    constexpr size_t kBudget = kCmdMaxPayload - 1024;
    // 全局顶点 → 本块局部顶点（跨块复用同一张表，用 stamp 判断有效性）
    std::vector<uint32_t> remap(verts.size(), UINT32_MAX);

    size_t tri = 0;
    while (tri + 3 <= indices.size()) {
        std::vector<TriMeshVertex> chunkVerts;
        std::vector<uint32_t> chunkIdx;
        std::vector<uint32_t> touched;   // 本块用过的全局顶点（用于复位 remap）
        size_t bytes = sizeof(TriMeshHeader);

        // 以**三角形**为单位装块：顶点按需搬入本块并重映射索引，
        // 保证每个 TriMesh 命令的索引都相对本块自己的顶点（0 起）
        while (tri + 3 <= indices.size()) {
            // 预估本三角形要新增的顶点数
            size_t newVerts = 0;
            for (int k = 0; k < 3; ++k) {
                const uint32_t gv = indices[tri + static_cast<size_t>(k)];
                if (gv >= verts.size()) {
                    newVerts = 0;   // 越界索引：终止本块（防御损坏输入）
                    break;
                }
                if (remap[gv] == UINT32_MAX) {
                    ++newVerts;
                }
            }
            const size_t addBytes = newVerts * sizeof(TriMeshVertex) + 3 * sizeof(uint32_t);
            if (!chunkIdx.empty() && bytes + addBytes > kBudget) {
                break;   // 本块已满 → 收尾，开新块
            }
            for (int k = 0; k < 3; ++k) {
                const uint32_t gv = indices[tri + static_cast<size_t>(k)];
                if (gv >= verts.size()) {
                    break;
                }
                if (remap[gv] == UINT32_MAX) {
                    remap[gv] = static_cast<uint32_t>(chunkVerts.size());
                    touched.push_back(gv);
                    chunkVerts.push_back(verts[gv]);
                }
                chunkIdx.push_back(remap[gv]);
            }
            bytes += addBytes;
            tri += 3;
        }
        if (chunkIdx.empty()) {
            break;   // 全是越界索引 → 不再前进，避免死循环
        }

        // 序列化本块：TriMeshHeader + 顶点 + 索引，尾部 16B 对齐
        TriMeshHeader hdr;
        hdr.vertexCount = static_cast<uint32_t>(chunkVerts.size());
        hdr.indexCount = static_cast<uint32_t>(chunkIdx.size());
        std::vector<uint8_t> payload;
        payload.reserve(bytes + kCmdAlign);
        const auto* hp = reinterpret_cast<const uint8_t*>(&hdr);
        payload.insert(payload.end(), hp, hp + sizeof(TriMeshHeader));
        for (const auto& v : chunkVerts) {
            const auto* vp = reinterpret_cast<const uint8_t*>(&v);
            payload.insert(payload.end(), vp, vp + sizeof(TriMeshVertex));
        }
        const auto* ip = reinterpret_cast<const uint8_t*>(chunkIdx.data());
        payload.insert(payload.end(), ip, ip + chunkIdx.size() * sizeof(uint32_t));
        while (payload.size() % kCmdAlign != 0) {
            payload.push_back(0);
        }
        append(Cmd::TriMesh, payload.data(), static_cast<uint32_t>(payload.size()));

        for (uint32_t gv : touched) {
            remap[gv] = UINT32_MAX;   // 复位，供下一块复用
        }
    }
}

void SceneCommandList::recordReplay(bool on) {
    recording_ = on;
    if (on) {
        replay_.clear();
        replayCursor_ = 0;
        replayFailed_ = false;
        hasMesh_ = false;
    }
}

bool SceneCommandList::beginReplay(bool allowMesh) {
    // 计划为空（无坐标命令）→ 不可重放。含 TriMesh 时默认拒绝：表面/QuickSurf 的
    // MC 拓扑会随坐标变化；只有调用方确认样式是"网格拓扑恒定"（Diamond/Plate/
    // 卡通等）时才允许，尺寸不符仍会在 append 处安全失败并回退全量重建。
    if (replay_.empty() || (hasMesh_ && !allowMesh) || pool_.empty()) {
        return false;
    }
    replaying_ = true;
    replayCursor_ = 0;
    replayFailed_ = false;
    return true;
}

bool SceneCommandList::endReplay() {
    replaying_ = false;
    // 必须恰好用完整份计划（少了 = 样式提前返回；多了/不符 = append 已置失败）
    const bool ok = !replayFailed_ && replayCursor_ == replay_.size();
    if (ok) {
        ++serial_;   // 坐标段已更新：serial 递增，后端据此重新解码/上传
    }
    return ok;
}

uint32_t SceneCommandList::appendString(std::string_view text) {
    const uint32_t offset = static_cast<uint32_t>(strings_.size());
    strings_.insert(strings_.end(), text.begin(), text.end());
    return offset;
}

bool SceneCommandList::empty() const { return pool_.empty(); }

uint32_t SceneCommandList::size() const {
    return static_cast<uint32_t>(pool_.size());
}

uint64_t SceneCommandList::serial() const { return serial_; }

bool SceneCommandList::first(CommandIterator& it) const {
    it = CommandIterator(*this);
    return !it.done();
}

bool SceneCommandList::next(CommandIterator& it) const {
    it.next();
    return !it.done();
}

void SceneCommandList::writeTo(ByteSink& out, const SceneCommandList& list) {
    // [0] u64 commandCount；[8] u64 poolSize；[16] pool；u64 stringPoolSize；strings
    // 命令条数：遍历池统计
    uint64_t commandCount = 0;
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        ++commandCount;
    }
    out.writeValue<uint64_t>(commandCount);
    out.writeValue<uint64_t>(list.pool_.size());
    out.write(list.pool_.data(), list.pool_.size());
    out.writeValue<uint64_t>(list.strings_.size());
    out.write(list.strings_.data(), list.strings_.size());
    out.alignTo(kCmdAlign);
}

bool SceneCommandList::readFrom(ByteSource& in, SceneCommandList& out) {
    // 顺序读取：commandCount | poolSize | pool | stringPoolSize | strings
    uint64_t commandCount = 0, poolSize = 0, stringPoolSize = 0;
    if (!in.readValue<uint64_t>(commandCount) ||
        !in.readValue<uint64_t>(poolSize) ||
        poolSize > kCmdMaxPayload * 4) {
        return false;
    }
    std::vector<uint8_t> pool(static_cast<size_t>(poolSize));
    if (!in.read(pool.data(), pool.size())) {
        return false;
    }
    if (!in.readValue<uint64_t>(stringPoolSize) || stringPoolSize > (1u << 24)) {
        return false;
    }
    std::vector<uint8_t> strings(static_cast<size_t>(stringPoolSize));
    if (!in.read(strings.data(), strings.size())) {
        return false;
    }
    in.alignTo(kCmdAlign);
    out.pool_ = std::move(pool);
    out.strings_ = std::move(strings);
    out.serial_ = 0;
    // 反序列化换掉了整块池：旧偏移记录全部失效（含 hasMesh_），必须重扫一遍。
    // 重放要求记录与池同源，此处清空即令 beginReplay 返回 false → 回退全量重建。
    out.replay_.clear();
    out.replayCursor_ = 0;
    out.recording_ = false;
    out.replaying_ = false;
    out.replayFailed_ = false;
    out.hasMesh_ = false;
    CommandIterator it(out);
    for (bool has = out.first(it); has; has = out.next(it)) {
        if (it.code() == Cmd::TriMesh) {
            out.hasMesh_ = true;
            break;
        }
    }
    return true;
}

} // namespace moleculens::engine
