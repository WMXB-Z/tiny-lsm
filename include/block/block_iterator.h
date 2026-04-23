#pragma once

#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "iterator/iterator.h"

namespace tiny_lsm {
class Block;
// =========================BlockIterator=======================
class BlockIterator {
public:
    // 标准迭代器类型定义
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<std::string, std::string>;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    // 构造函数(默认参数在头文件中的函数声明中给出，实现中不给出)
    BlockIterator(std::shared_ptr<Block> b, size_t index, uint64_t tranc_id, bool keep_all_versions = false);
    BlockIterator(std::shared_ptr<Block> b, const std::string& key, uint64_t tranc_id, bool keep_all_versions = false);
    // BlockIterator(std::shared_ptr<Block> b, uint64_t tranc_id);
    BlockIterator(): block(nullptr), current_index(0), tranc_id_(0) {}  // end iterator

    // 迭代器操作
    pointer operator->() const;
    BlockIterator& operator++();
    BlockIterator operator++(int) = delete;
    bool operator==(const BlockIterator& other) const;
    bool operator!=(const BlockIterator& other) const;
    value_type operator*() const;
    bool is_end();
    uint64_t get_cur_tranc_id() const;

private:
    void update_current() const;
    // 跳过当前不可见事务的id (如果开启了事务功能)
    void skip_by_tranc_id();

private:
    // 如果一个 Block 对象本来是被 std::shared_ptr 管理的，
    // 那么在类的内部想拿到“指向自己的 shared_ptr”，不能直接用this，
    // 必须通过 enable_shared_from_this 提供的 shared_from_this()。
    std::shared_ptr<Block> block;  // !指向所属的 Block实例

    size_t current_index;          // 当前位置的索引
    uint64_t tranc_id_;            // 当前事务 id
    mutable std::optional<value_type>
        cached_value;  // 缓存当前值，避免频繁查找Block导致的频繁解码
    bool keep_all_versions_ = false;
};
}  // namespace tiny_lsm
