// =============================================================================
// MolecuLens · entity · string_pool.h —— 字符串驻留（StringPool）
// 规格来源：主方案 §4.5（重复字符串映射为稳定 uint32 索引）
// 依赖约束：仅 <string>/<string_view>/<unordered_map>/<vector>
// =============================================================================

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace moleculens::entity {

// 字符串驻留：所有重复字符串（原子名/残基名/链名/文件名）经此映射为 uint32 索引。
// 索引稳定（不随插入变化）；resolve 返回的 string_view 生命周期同本池。
class StringPool {
public:
    uint32_t intern(std::string_view text) {
        auto it = index_.find(text);
        if (it != index_.end()) {
            return it->second;
        }
        uint32_t id = static_cast<uint32_t>(storage_.size());
        storage_.emplace_back(text);
        index_.emplace(storage_.back(), id);
        return id;
    }

    std::string_view resolve(uint32_t id) const {
        if (id >= storage_.size()) {
            return {};
        }
        return storage_[id];
    }

    size_t size() const { return storage_.size(); }

private:
    std::vector<std::string> storage_;
    std::unordered_map<std::string_view, uint32_t> index_;
};

} // namespace moleculens::entity
