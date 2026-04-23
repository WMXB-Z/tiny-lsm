#include "lsm/two_merge_iterator.h"

namespace tiny_lsm {

TwoMergeIterator::TwoMergeIterator() {}

TwoMergeIterator::TwoMergeIterator(std::shared_ptr<BaseIterator> it_a,
                                   std::shared_ptr<BaseIterator> it_b,
                                   uint64_t max_tranc_id,
                                   bool keep_all_versions)
    : it_a(std::move(it_a)),
      it_b(std::move(it_b)),
      max_tranc_id_(max_tranc_id),
      keep_all_versions_(keep_all_versions) {
    skip_by_tranc_id();  // 先跳过不可见的事务
    skip_it_b();               // 跳过与 it_a 重复的 key
    choose_a = choose_it_a();  // 决定使用哪个迭代器
}

bool TwoMergeIterator::choose_it_a() {
    // TODO: Lab 4.4: 实现选择迭代器的逻辑
    // 判断当前解引用应该使用哪个迭代器
    if (it_a->is_end()) {
        return false;
    }
    if (it_b->is_end()) {
        return true;
    }
    auto key_a = (**it_a).first;
    auto key_b = (**it_b).first;
    if (key_a != key_b) {
        return key_a < key_b;  // 比较 key
    }
    // 相同键：在keep_all_versions模式下，首先发出较大的tranc_id，
    // 以便区块条目按tranc_id降序排列，这是Block::adjust_idx_by_tranc_id所期望的。
    if (keep_all_versions_) {
        return it_a->get_tranc_id() > it_b->get_tranc_id();
    }
    // 想当时偏向选it_a，因为it_a是上层的迭代器，而上层的数据会版本会更新
    return true;
}

void TwoMergeIterator::skip_it_b() {
    if (keep_all_versions_) {
        return;
    }
    if (!it_a->is_end() && !it_b->is_end() &&
        (**it_a).first == (**it_b).first) {
        ++(*it_b);
    }
}

void TwoMergeIterator::skip_by_tranc_id() {
    // TODO:根据事务可见性进行滤除的辅助函数
    if (max_tranc_id_ == 0) {
        return;
    }
    while (it_a->get_tranc_id() > max_tranc_id_) {
        ++(*it_a);
    }
    while (it_b->get_tranc_id() > max_tranc_id_) {
        ++(*it_b);
    }
}

// !合并过程中实现层之间元素的去重
// !level-1以下的层内元素是无重key的，不用层内部去重。而level-0的层内去重是通过迭代器HeapIterator实现的！
BaseIterator& TwoMergeIterator::operator++() {
    // TODO: Lab 4.4: 实现 ++ 重载
    if (choose_a) {
        ++(*it_a);
    } else {
        ++(*it_b);
    }

    skip_by_tranc_id();     // 先跳过不可见的事务
    skip_it_b();               // 跳过iter-b中重复的key
    choose_a = choose_it_a();  // 重新决定使用哪个迭代器
    return *this;
}

bool TwoMergeIterator::operator==(const BaseIterator& other) const {
    // TODO: Lab 4.4: 实现 == 重载
    if (other.get_type() != IteratorType::TwoMergeIterator) {
        return false;
    }
    auto other2 = dynamic_cast<const TwoMergeIterator&>(other);
    if (this->is_end() && other2.is_end()) {
        return true;
    }
    if (this->is_end() || other2.is_end()) {
        return false;
    }
    return it_a == other2.it_a && it_b == other2.it_b &&
           choose_a == other2.choose_a;
}

bool TwoMergeIterator::operator!=(const BaseIterator& other) const {
    // TODO: Lab 4.4: 实现 != 重载
    return !(*this == other);
}

BaseIterator::value_type TwoMergeIterator::operator*() const {
    // TODO: Lab 4.4: 实现 * 重载
    if (choose_a) {
        return **it_a;
    } else {
        return **it_b;
    }
}

TwoMergeIterator::pointer TwoMergeIterator::operator->() const {
    // TODO: Lab 4.4: 实现 -> 重载
    update_current();
    return current.get();
}

IteratorType TwoMergeIterator::get_type() const {
    return IteratorType::TwoMergeIterator;
}

uint64_t TwoMergeIterator::get_tranc_id() const {
    if (keep_all_versions_) {
        if (choose_a && it_a && !it_a->is_end()) {
            return it_a->get_tranc_id();
        }
        if (!choose_a && it_b && !it_b->is_end()) {
            return it_b->get_tranc_id();
        }
    }
    return max_tranc_id_;
}

bool TwoMergeIterator::is_end() const {
    if (it_a == nullptr && it_b == nullptr) {
        return true;
    }
    if (it_a == nullptr) {
        return it_b->is_end();
    }
    if (it_b == nullptr) {
        return it_a->is_end();
    }
    return it_a->is_end() && it_b->is_end();
}

bool TwoMergeIterator::is_valid() const {
    if (it_a == nullptr && it_b == nullptr) {
        return false;
    }
    if (it_a == nullptr) {
        return it_b->is_valid();
    }
    if (it_b == nullptr) {
        return it_a->is_valid();
    }
    return it_a->is_valid() || it_b->is_valid();
}


void TwoMergeIterator::update_current() const {
    // TODO: Lab 4.4: 实现更新缓存键值对的辅助函数
    if (choose_a) {
        current = std::make_shared<value_type>(**it_a);
    } else {
        current = std::make_shared<value_type>(**it_b);
    }
}
}  // namespace tiny_lsm