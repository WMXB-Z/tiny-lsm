#include "iterator/iterator.h"

#include <tuple>
#include <vector>

namespace tiny_lsm {

// *************************** SearchItem ***************************
bool operator<(const SearchItem& a, const SearchItem& b) {
    // TODO: Lab2.2 实现比较规则< (个人实现已通过)
    if (a.key_ != b.key_) return a.key_ < b.key_;
    if (a.tranc_id_ != b.tranc_id_) return a.tranc_id_ > b.tranc_id_;
    if (a.level_ != b.level_) return a.level_ < b.level_;
    return a.idx_ < b.idx_;
}

bool operator>(const SearchItem& a, const SearchItem& b) {
    // TODO: Lab2.2 实现比较规则> (个人实现已通过)
    if (a.key_ != b.key_) return a.key_ > b.key_;
    if (a.tranc_id_ != b.tranc_id_) return a.tranc_id_ < b.tranc_id_;
    if (a.level_ != b.level_) return a.level_ > b.level_;
    return a.idx_ > b.idx_;
}

bool operator==(const SearchItem& a, const SearchItem& b) {
    // TODO: Lab2.2 实现比较规则==（已通过）
    // 不是比较这两个元素是不是完全相同版本，仅是判断两个元素"同key+同源"
    return a.idx_ == b.idx_ && a.key_ == b.key_;
}

// *************************** HeapIterator ***************************
HeapIterator::HeapIterator(bool skip_delete, bool keep_all_versions)
    : skip_delete_(skip_delete), keep_all_versions_(keep_all_versions) {
    // TODO: Lab2.2 实现 HeapIterator 构造函数（已通过）
}

HeapIterator::HeapIterator(std::vector<SearchItem> item_vec,
                           uint64_t max_tranc_id, bool skip_delete,
                           bool keep_all_versions)
    : max_tranc_id_(max_tranc_id), skip_delete_(skip_delete), keep_all_versions_(keep_all_versions) {
    // TODO: Lab2.2 实现 HeapIterator 构造函数，根据memtable搜索所得的vector构造（已通过）
    for (auto& item : item_vec) {
        items.push(item);   // 构建大根堆
    }

    // 保证堆根元素的"当前 top 是否"整体合法"
    // 若堆根元素不合法，则要做清除，直到top合法为止。
    // （lazy heap:heap 里可能有垃圾数据，但每次保证top一定是合法的）
    while (!top_value_legal()) {
        // 1. 先跳过事务id不可见的元素部分
        skip_by_tranc_id();

        // skip_delete用于控制迭代器是否删除value为空的键值对
        // true表示要删除，应用于用户查询数据时
        // false表示要不删，应用于数据落盘时
        if (!skip_delete_) {
            continue;
        }

        // 2. 进过上述判断处理，进入此部分的不合法情况只能是：事务id上合法，skip_delet=ture，而value为空
        // 注意，这类情况中，需要删除同key中被该版本覆盖的旧版本
        while (!items.empty() && items.top().value_.empty()) {
            auto del_key = items.top().key_;
            if (!keep_all_versions_) {
                while (!items.empty() && items.top().key_ == del_key) {
                    items.pop();
                }
            } else {
                items.pop();
            }
        }
    }
}

HeapIterator::pointer HeapIterator::operator->() const {
    // 返回std::pair<std::string, std::string>*
    // TODO: Lab2.2 实现 -> 重载(已通过)
    update_current();
    return current.get();   //通过shared_ptr返回裸指针做临时使用
}

HeapIterator::value_type HeapIterator::operator*() const {
    // TODO: Lab2.2 实现 * 重载（已通过）
    return std::make_pair(items.top().key_, items.top().value_);
}

BaseIterator& HeapIterator::operator++() {
    // TODO: Lab2.2 实现 ++ 重载（已通过）
    // ? 这里的++是通过对heap的指向后移来实现的
    // 1、自增后的key不能是之前相同的key, 如果是(以为着实际上被前者覆写了), 则跳过
    // 2、自增后的键值对不能是 删除标记+value为空
    if (items.empty()) {
        return *this;  // 处理空队列情况
    }

    auto old_item = items.top();
    items.pop();    // 指向后移

    // keep_all_versions_=false，则删除与top相同key的次优先级版本
    if (!keep_all_versions_) {
        while (!items.empty() && items.top().key_ == old_item.key_) {
            items.pop();
        }
    }

    // 与构造函数相同, 下一个key中事务不可见部分和删除的元素需要跳过
    while (!top_value_legal()) {
        // 1. 先跳过事务 id 不可见的部分
        skip_by_tranc_id();

        if (!skip_delete_) {
            continue;
        }
        // 2. 跳过标记为删除的元素
        while (!items.empty() && items.top().value_.empty()) {
            // 如果当前元素的value为空，则说明该元素已经被删除，需要从优先队列中删除
            auto del_key = items.top().key_;
            if (!keep_all_versions_) {
                while (!items.empty() && items.top().key_ == del_key) {
                    items.pop();
                }
            } else {
                items.pop();
            }
        }
    }
    return *this;
}

bool HeapIterator::operator==(const BaseIterator& other) const {
    // TODO: Lab2.2 实现 == 重载（已通过）
    if (other.get_type() != IteratorType::HeapIterator) {
        return false;
    }
    auto other2 = dynamic_cast<const HeapIterator&>(other);
    if (items.empty() && other2.items.empty()) {
        return true;
    }
    if (items.empty() || other2.items.empty()) {
        return false;
    }
    return items.top().key_ == other2.items.top().key_ &&
           items.top().value_ == other2.items.top().value_;
}

bool HeapIterator::operator!=(const BaseIterator& other) const {
    // TODO: Lab2.2 实现 != 重载（已通过）
    return !(*this == other);
}

// 大根堆的根元素的合法性判断，以下非法：
// 事务id>上界max_tranc_id
// 要跳过删除记录+value为空
// 非法元素在构建迭代器时，会被跳过
bool HeapIterator::top_value_legal() const {
    // TODO: Lab2.2 判断顶部元素是否合法（个人实现已通过）
    // ? 被删除的值是不合法
    // ? 不允许访问的事务创建或更改的键值对不合法(暂时忽略)
    if (items.empty()) //空队列是合法的
        return true;

    if (max_tranc_id_ == 0) {
        // 没有开启事务
        // 不为空的 value 才合法
        if (skip_delete_) {
            //如果这个迭代器需要跳过删除记录
            return items.top().value_.size() > 0;
        }
        //否则，就关于top_value是否合法，只需要在意事务id就好了，那么就会是true
        return true;
    }

    if (items.top().tranc_id_ <= max_tranc_id_) {
        // 事务id可见, 则判断其value是否为空
        if (skip_delete_) {
            //如果这个迭代器需要跳过删除记录
            return items.top().value_.size() > 0;
        }
        //否则，就关于top_value是否合法，只需要在意事务id就好了，那么就会是true
        return true;
    } else {
        // 事务id不可见, 即不合法
        return false;
    }
}

// 跳过tranc_id中不合法的元素
void HeapIterator::skip_by_tranc_id() {
    // TODO: Lab2.2 后续的Lab实现, 只是作为标记提醒（已通过）
    if (max_tranc_id_ == 0) // 没有开启事务
        return;
    // 开启了事务，对事务值非法的元素做清除
    while (!items.empty() && items.top().tranc_id_ > max_tranc_id_) {
        items.pop();
    }
}

bool HeapIterator::is_end() const { return items.empty(); }
bool HeapIterator::is_valid() const { return !items.empty(); }

void HeapIterator::update_current() const {
    // current 缓存了当前键值对的值, 实现 -> 重载时需要
    // TODO: Lab2.2 更新当前缓存值(已通过)
    if (!items.empty()) {
        current = std::make_shared<value_type>(items.top().key_, items.top().value_);
    } else {
        current.reset();    //释放当前shared_ptr持有的对象（让它变成空指针）
    }
}

IteratorType HeapIterator::get_type() const {
    return IteratorType::HeapIterator;
}

uint64_t HeapIterator::get_tranc_id() const {
    if (keep_all_versions_ && !items.empty()) {
        return items.top().tranc_id_;
    }
    return max_tranc_id_;
}
}  // namespace tiny_lsm
