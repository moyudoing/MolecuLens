// =============================================================================
// MolecuLens · engine · cmd/command_list.cpp —— SceneCommandList 实现
// 规格来源：主方案 §5.3 / 附录 F（16B 对齐内存池；字符串池；serial；
//           迭代器；序列化 writeTo/readFrom 字节布局）
// =============================================================================

#include "engine/cmd/cmd.h"

#include <cstring>

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
        done_ = true;
        return;
    }
    done_ = false;
}

void SceneCommandList::append(Cmd code, const void* payload, uint32_t bytes) {
    // 16B 对齐填充（前一条命令尾部）
    while (pool_.size() % kCmdAlign != 0) {
        pool_.push_back(0);
    }
    CmdHeader header;
    header.code = code;
    header.pad[0] = header.pad[1] = header.pad[2] = 0;
    header.size = bytes;
    const uint8_t* p = static_cast<const uint8_t*>(payload);
    pool_.insert(pool_.end(), reinterpret_cast<const uint8_t*>(&header),
                 reinterpret_cast<const uint8_t*>(&header) + sizeof(CmdHeader));
    pool_.insert(pool_.end(), p, p + bytes);
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
    return true;
}

} // namespace moleculens::engine
