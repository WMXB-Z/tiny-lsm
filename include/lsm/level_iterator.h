#pragma once
#include <memory>
#include <optional>
#include <shared_mutex>

#include "iterator/iterator.h"

namespace tiny_lsm {
class LSMEngine;

class Level_Iterator : public BaseIterator {
public:
    Level_Iterator() = default;
    Level_Iterator(std::shared_ptr<LSMEngine> engine_, uint64_t max_tranc_id);

    virtual BaseIterator& operator++() override;
    virtual bool operator==(const BaseIterator& other) const override;
    virtual bool operator!=(const BaseIterator& other) const override;
    virtual value_type operator*() const override;
    virtual IteratorType get_type() const override;
    virtual uint64_t get_tranc_id() const override;
    virtual bool is_end() const override;
    virtual bool is_valid() const override;

    BaseIterator::pointer operator->() const;

private:
    std::shared_ptr<LSMEngine> engine_; //上层LSMEngine的智能指针engine_
    //  存放不同的Level的层间迭代器(HeapIterator、ConcactIterator)的数组
    std::vector<std::shared_ptr<BaseIterator>> iter_vec;   //存放内存层+外存L0-Ln层的迭代器的指针数组
    size_t cur_idx_;        // 当前使用中选中的迭代器（heap、l0的concact、l1的concact...）索引
    uint64_t max_tranc_id_; // 可见的事务的最大id
    mutable std::optional<value_type> cached_value;  // 缓存当前访问的key-value
    std::shared_lock<std::shared_mutex> rlock_;

private:
    void update_current() const;
    std::pair<size_t, std::string> get_min_key_idx() const;
    void skip_key(const std::string& key);
};
}  // namespace tiny_lsm
