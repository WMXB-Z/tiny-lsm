#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "block/block_iterator.h"
namespace tiny_lsm {

// class SstIterator;
class SST;

// std::optional<std::pair<SstIterator, SstIterator>> sst_iters_monotony_predicate(
//     std::shared_ptr<SST> sst, uint64_t tranc_id,
//     std::function<int(const std::string&)> predicate);

class SstIterator : public BaseIterator {
    // 友元函数
    // friend std::optional<std::pair<SstIterator, SstIterator>> sst_iters_monotony_predicate(
    //     std::shared_ptr<SST> sst, uint64_t tranc_id,
    //     std::function<int(const std::string&)> predicate);
    
    // 友元类
    friend SST;

private:    
    std::shared_ptr<SST> m_sst; // 记录原始的SST类对象
    int64_t m_block_idx;    // 记录当前读入的Block在SST中的位置
    uint64_t max_tranc_id_; // 记录当前Block中事务id的最大值
    std::shared_ptr<BlockIterator> m_block_it;  // 记录当前读入的Block的某个key的迭代器位置
    mutable std::optional<value_type> cached_value;  // 缓存当前值
    bool keep_all_versions_ = false;

    void update_current() const;
    void set_block_idx(size_t idx);
    void set_block_it(std::shared_ptr<BlockIterator> it);

public:
    SstIterator(){}
    // 创建迭代器,并移动到第一个key
    SstIterator(std::shared_ptr<SST> sst, uint64_t tranc_id, bool keep_all_versions = false);
    // 创建迭代器并移动到第指定key
    SstIterator(std::shared_ptr<SST> sst, const std::string& key, uint64_t tranc_id, bool keep_all_versions = false);

    std::optional<std::pair<SstIterator, SstIterator>> sst_iters_monotony_predicate( std::shared_ptr<SST> sst, uint64_t tranc_id, std::function<int(const std::string&)> predicate);
    std::optional<std::pair<SstIterator, SstIterator>> iters_monotony_predicate(std::shared_ptr<SST> sst, uint64_t tranc_id, const std::string& preffix);

    void seek_first();
    void seek(const std::string& key);
    std::string key();
    std::string value();

    virtual BaseIterator& operator++() override;
    virtual bool operator==(const BaseIterator& other) const override;
    virtual bool operator!=(const BaseIterator& other) const override;
    virtual value_type operator*() const override;
    virtual IteratorType get_type() const override;
    virtual uint64_t get_tranc_id() const override;
    virtual bool is_end() const override;
    virtual bool is_valid() const override;

    pointer operator->() const;
    uint64_t get_cur_tranc_id() const;

    static std::pair<HeapIterator, HeapIterator> merge_sst_iterator(
        std::vector<SstIterator> iter_vec, uint64_t tranc_id,
        bool keep_all_versions = false);
};
}  // namespace tiny_lsm