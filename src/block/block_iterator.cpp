#include "block/block_iterator.h"

#include <cstdint>
#include <memory>
#include <stdexcept>

#include "block/block.h"

class Block;

namespace tiny_lsm {
BlockIterator::BlockIterator(std::shared_ptr<Block> b, size_t index, uint64_t tranc_id, bool keep_all_versions)
    : block(b),
      current_index(index),
      tranc_id_(tranc_id),
      cached_value(std::nullopt),
      keep_all_versions_(keep_all_versions) {
    skip_by_tranc_id();
}

// BlockIterator构造时，定位到该data block指定的key上
BlockIterator::BlockIterator(std::shared_ptr<Block> b, const std::string& key, uint64_t tranc_id, bool keep_all_versions)
    : block(b),
      tranc_id_(tranc_id),
      cached_value(std::nullopt),
      keep_all_versions_(keep_all_versions) {
    // TODO: Lab3.2 创建迭代器时直接移动到指定的key位置（已通过）
    // ? 借助之前实现的 Block 类的成员函数get_idx_binary查找key在offsets中索引
    // ? 查找结果在合法，则记录在current_index中；非法，则current_index=offsets.size()，即数组有效界外
    auto key_idx_ops = block->get_idx_binary(key, tranc_id);
    if (key_idx_ops.has_value()) {
        current_index = key_idx_ops.value();
    } else {
        current_index = block->offsets.size();
    }
}

// BlockIterator::BlockIterator(std::shared_ptr<Block> b, uint64_t tranc_id)
//     : block(b), current_index(0), tranc_id_(tranc_id), cached_value(std::nullopt) {
//   skip_by_tranc_id();
// }

// 语法规定：operator->() 必须返回“指针-like东西”
// 也就是说它返回的通常是：
// 原始指针 T*
// 或者重载了 operator->() 的对象
// 实际上 ->是单目运算符，解析过程会链式返回operator->()，直至返回指针类型
// 例如：a->b = ((a.operator->())->b) = (c->b) = (c.operator->())->b = .... = (*e).b
BlockIterator::pointer BlockIterator::operator->() const {
    // TODO: Lab3.2 -> 重载（已通过）
    update_current();
    // *：从 optional 里“取出值”
    // &：对 pair 引用取地址
    return &(*cached_value);    
}

BlockIterator& BlockIterator::operator++() {
    // TODO: Lab3.2 ++ 重载（已通过）
    // ? 这里的++本质上通过对offsets的指向后移来实现的
    // ? 在后续的Lab实现事务后，需要对这个函数进行返修
    if (block && current_index < block->size()) {
        auto prev_idx = current_index;
        auto prev_offset = block->get_offset_at(prev_idx);
        auto prev_entry = block->get_entry_at(prev_offset);

        ++current_index;

        // 1、跳过相同的key
        if (!keep_all_versions_) {
            while (block && current_index < block->size()) {
                auto cur_offset = block->get_offset_at(current_index);
                auto cur_entry = block->get_entry_at(cur_offset);
                if (cur_entry.key != prev_entry.key) {
                    break;
                }
                // 可能会连续出现多个key, 但由不同事务创建, 相同的key直接跳过
                ++current_index;
            }
        }

        // 2、出现不同的key时, 还需要跳过不可见事务的键值对
        skip_by_tranc_id();
    }
    return *this;
}

bool BlockIterator::operator==(const BlockIterator& other) const {
    // TODO: Lab3.2 == 重载（已通过）
    if (block == nullptr && other.block == nullptr) {
        return true;
    }
    if (block == nullptr || other.block == nullptr) {
        return false;
    }
    return (block == other.block) && (current_index == other.current_index) ;
}

bool BlockIterator::operator!=(const BlockIterator& other) const {
    // TODO: Lab3.2 != 重载（已通过）
    return !(*this == other);
}

BlockIterator::value_type BlockIterator::operator*() const {
    // TODO: Lab3.2 * 重载（已通过）
    if (!block || current_index >= block->size()) {
        throw std::out_of_range("Iterator out of range");
    }

    // 使用缓存避免重复解析
    if (!cached_value.has_value()) {
        size_t offset = block->get_offset_at(current_index);
        cached_value = std::make_pair(block->get_key_at(offset), block->get_value_at(offset));
    }
    return *cached_value;
}

bool BlockIterator::is_end() { return current_index == block->offsets.size(); }

uint64_t BlockIterator::get_cur_tranc_id() const {
    if (!block || current_index >= block->offsets.size()) {
        return 0;
    }
    size_t offset = block->get_offset_at(current_index);
    return block->get_tranc_id_at(offset);
}

// current_index表示offsets的某个下标
// cached_value缓存current_index对应的k-v
void BlockIterator::update_current() const {
    // TODO: Lab3.2 更新当前指针（已通过）
    // ? 该函数是可选的实现, 你可以采用自己的其他方案实现->, 而不是使用 cached_value 来缓存当前指针
    if (!cached_value && current_index < block->offsets.size()) {
        size_t offset = block->get_offset_at(current_index);
        cached_value = std::make_pair(block->get_key_at(offset), block->get_value_at(offset));
    }
}

// 访问offsets过程中，跳过不符合trans_id要求的key
void BlockIterator::skip_by_tranc_id() {
    // TODO: Lab3.2 * 跳过事务ID（已通过）
    if (tranc_id_ == 0) {
        // 没有开启事务功能
        cached_value = std::nullopt;
        return;
    }

    while (current_index < block->offsets.size()) {
        size_t offset = block->get_offset_at(current_index);
        auto tranc_id = block->get_tranc_id_at(offset);
        if (tranc_id <= tranc_id_) {
            // 位置合法
            break;
        }
        // 否则跳过不可见事务的键值对
        ++current_index;
    }
    cached_value = std::nullopt;
}
}  // namespace tiny_lsm