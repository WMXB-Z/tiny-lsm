#pragma once

#include <memory>
#include <vector>

#include "sst.h"
#include "sst_iterator.h"

namespace tiny_lsm {
class ConcactIterator : public BaseIterator {
private:
    SstIterator cur_iter;   // 当前指向SST的迭代器
    size_t cur_idx;  // 不是真实的sst_id, 而是当前迭代器指向的SST在ssts中的索引
    std::vector<std::shared_ptr<SST>> ssts; // 存放这一level层所有SST句柄的数组
    uint64_t max_tranc_id_; // 调用该迭代器的事务id，即最大事务的可见范围
    bool keep_all_versions_ = false;    // 是否保留key的重复版本

public:
    ConcactIterator(std::vector<std::shared_ptr<SST>> ssts, uint64_t tranc_id,
                    bool keep_all_versions = false);

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
};
}  // namespace tiny_lsm
