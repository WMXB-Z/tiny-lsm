#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>

namespace tiny_lsm {

// 各类迭代器
enum class IteratorType {
    SkipListIterator,
    MemTableIterator,
    SstIterator,
    HeapIterator,
    TwoMergeIterator,
    ConcactIterator,
    LevelIterator,
};

// ************************ BaseIterator迭代器基类 ************************
class BaseIterator {
public:
    using value_type = std::pair<std::string, std::string>; // 迭代器当前访问的键值对数据
    using pointer = value_type*;    //键值对数据的指针
    using reference = value_type&;  //键值对数据的引用

    virtual BaseIterator& operator++() = 0;
    virtual bool operator==(const BaseIterator& other) const = 0;
    virtual bool operator!=(const BaseIterator& other) const = 0;
    virtual value_type operator*() const = 0;
    virtual IteratorType get_type() const = 0;
    virtual uint64_t get_tranc_id() const = 0;
    virtual bool is_end() const = 0;
    virtual bool is_valid() const = 0;
};

class SstIterator;
// *************************** SearchItem ***************************
struct SearchItem {
    std::string key_;
    std::string value_;
    uint64_t tranc_id_; //事务ID，表示这个 key-value 是在哪个“时间点/事务”产生的
    int idx_;       // 当前元素来自“第几个输入源”(用于区分同一层级不同表)
    int level_;  // 来自sst的level(用于区分不同LSM层级)

    SearchItem() = default;
    SearchItem(std::string k, std::string v, int i, int l, uint64_t tranc_id)
        : key_(std::move(k)),
          value_(std::move(v)),
          idx_(i),
          level_(l),
          tranc_id_(tranc_id) {}
};

bool operator<(const SearchItem& a, const SearchItem& b);
bool operator>(const SearchItem& a, const SearchItem& b);
bool operator==(const SearchItem& a, const SearchItem& b);

// *************************** HeapIterator ***************************

class HeapIterator : public BaseIterator {
    friend class SstIterator;

public:
    // HeapIterator::HeapIterator() = default;不能显示添加无参构造器，和下面的默认构造函数二选一
    HeapIterator(bool skip_delete = true, bool keep_all_versions = false);
    HeapIterator(std::vector<SearchItem> item_vec, uint64_t max_tranc_id,
                 bool skip_delete = true, bool keep_all_versions = false);
    pointer operator->() const;
    virtual value_type operator*() const override;
    BaseIterator& operator++() override;
    BaseIterator operator++(int) = delete;
    virtual bool operator==(const BaseIterator& other) const override;
    virtual bool operator!=(const BaseIterator& other) const override;

    virtual IteratorType get_type() const override;
    virtual uint64_t get_tranc_id() const override;
    virtual bool is_end() const override;
    virtual bool is_valid() const override;

private:
    bool top_value_legal() const;
    // 跳过当前不可见事务的id (如果开启了事务功能)
    void skip_by_tranc_id();
    void update_current() const;

private:
    // priority_queue默认大根堆，这里指定是小根堆，第三参数：表示谁应该被压下去（less:小的压下面；greater大的压下面）
    std::priority_queue<SearchItem, std::vector<SearchItem>,
                        std::greater<SearchItem>> items;    
    // mutable：允许这个成员变量在 const 成员函数中被修改
    mutable std::shared_ptr<value_type> current;  // 用于存储当前元素。
    uint64_t max_tranc_id_ = 0;
    bool skip_delete_;      //用于控制迭代器是否删除value为空的键值对
    bool keep_all_versions_ = false;    //用户是否能看到大根堆中的同元素的重复版本
};
}  // namespace tiny_lsm