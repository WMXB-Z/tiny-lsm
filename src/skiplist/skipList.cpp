#include "skiplist/skiplist.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace tiny_lsm {

// ************************ SkipListIterator ************************
BaseIterator& SkipListIterator::operator++() {
    // TODO: 实现SkipListIterator的++操作符
    // ? current 是当前节点指针, forward_[0] 是最底层链表的下一个节点
    if (current) {
        current = current->forward_[0];
    }
    return *this;
}

bool SkipListIterator::operator==(const BaseIterator& other) const {
    // TODO: 实现SkipListIterator的==操作符
    // ? 需要先通过 get_type() 判断类型再做 dynamic_cast
    if (other.get_type() != IteratorType::SkipListIterator) 
        return false;
    auto other_cast = dynamic_cast<const SkipListIterator&>(other);  // 继承关系中类型转换
    return current == other_cast.current;
}

bool SkipListIterator::operator!=(const BaseIterator& other) const {
    // TODO: 实现SkipListIterator的!=操作符
    return !(*this == other); //这里直接!(operator==)
}

SkipListIterator::value_type SkipListIterator::operator*() const {
    // TODO: 实现SkipListIterator的*操作符
    // ? 若 current 为空需抛出异常
    if (!current) 
        throw std::runtime_error("Dereferencing invalid iterator");
    return {current->key_, current->value_};
}


// ************************ SkipList ************************
// 构造函数
SkipList::SkipList(int max_lvl) : max_level(max_lvl), current_level(1) {
    head = std::make_shared<SkipListNode>("", "", max_level, 0);
    dis_01 = std::uniform_int_distribution<>(0, 1);
    dis_level = std::uniform_int_distribution<>(0, (1 << max_lvl) - 1);
    gen = std::mt19937(std::random_device()());
}

// 生成跳表的随机高
int SkipList::random_level() {
    // ? 通过"抛硬币"的方式随机生成层数：
    // ? - 每次有50%的概率增加一层
    // ? - 确保层数分布为：第1层100%，第2层50%，第3层25%，以此类推
    // ? - 层数范围限制在[1, max_level]之间，避免浪费内存
    // TODO: 插入时随机为这一次操作确定其最高连接的链表层数(y)
    int level_num = 1;
    while ((std::rand() & 0x01) == 1 && level_num < max_level) {
        ++level_num;
    }
    return level_num;
}

// 插入或更新键值对
void SkipList::put(const std::string& key, const std::string& value, uint64_t tranc_id) {
    // 用spdlog以 trace（最详细级别）打印一条格式化日志
    spdlog::trace("SkipList--put({}, {}, {})", key, value, tranc_id);
    // TODO:实现插入或更新键值对
    // ? Hint: 你需要保证不同`Level`的步长从底层到高层逐渐增加
    // ? 你可能需要使用到`random_level`函数以确定层数,其注释中为你提供一种思路
    // ? tranc_id 为事务id, 直接将其传递到 SkipListNode 的构造函数中即可
    // ? 若key存在且tranc_id相同, 仅更新value; 否则插入新节点
    // ? 注意维护 size_bytes

    std::vector<std::shared_ptr<SkipListNode>> update_nodes(max_level, nullptr);
    // （1）从高层起查找（这样最后的插入位置一定是第1层，便于先一步判断元素存在性）插入位置并实现插入
    auto cmp = [](const auto& a1, const auto& a2, const auto& b1, const auto& b2) {
        if (a1 == a2) {
            return b1 > b2;  // a 小的在前（a 大的“更大”）
        }
        return a1 < a2;  // a 相等时比较 b
    };

    std::shared_ptr<SkipListNode> tmp = head;
    int now_level = random_level();
    auto new_node = std::make_shared<SkipListNode>(key, value, now_level, tranc_id);
    for (int i = current_level - 1; i >= 0; i--) {
        // 依次遍历所有层，在第i层向右扫描，直到找到目标节点或遍历完
        while (tmp->forward_[i] && cmp(tmp->forward_[i]->key_, key, tmp->forward_[i]->tranc_id_, tranc_id)) {
            tmp = tmp->forward_[i];
        }

        spdlog::trace("SkipList--put({}, {}, {}), level{} needs updating", key, value, tranc_id, i);
        update_nodes[i] = tmp;  // 每次记录插入位置的前节点
    }

    // （2）判断元素存在性，存在则直接更新值(这里可以直接使用get()函数判断)
    tmp = tmp->forward_[0];
    if (tmp && tmp->key_ == key && tmp->tranc_id_ == tranc_id) {
        // 若 key 存在且 tranc_id 相同，更新 value
        size_bytes += value.size() - tmp->value_.size();
        tmp->value_ = value;
        tmp->tranc_id_ = tranc_id;
        spdlog::trace(
            "SkipList--put({}, {}, {}), key and tranc_id_ is the same, "
            "only update value to {}", key, value, tranc_id, value);
        return;
    }

    // 步骤2：随机生成新节点存放的层级，如果该层级 > 当前最高层数，则跳表扩展
    if (now_level > current_level) {
        for (int i = current_level; i < now_level; i++) {
            update_nodes[i] = head;
            spdlog::trace("SkipList--put({}, {}, {}), update level{} to head", key, value, tranc_id, i);
        }
        current_level = now_level;  // 更新目前双向链表的高度
    }

    // 步骤3：插入并更新各个相关节点
    for (int i = 0; i < now_level; i++) {
        // 依据update_nodes中的记录进行更新
        new_node->forward_[i] = update_nodes[i]->forward_[i];
        if (new_node->forward_[i]) {  // 有后面节点，说明要跟新前驱和后继指针
            new_node->forward_[i]->backward_[i] = new_node;
        }
        new_node->backward_[i] = update_nodes[i];
        update_nodes[i]->forward_[i] = new_node;
    }
    size_bytes += key.size() + value.size() + sizeof(uint64_t);
}

// 查找键值对
SkipListIterator SkipList::get(const std::string& key, uint64_t tranc_id) {
    spdlog::trace("SkipList--get({}) called", key);
    // TODO: 实现查找键值对
    // ? 从最高层开始向下查找, 最终在底层确认 key 是否存在
    // ? 若 tranc_id == 0, 直接比较 key 返回; 否则需满足事务可见性 (tranc_id_<= tranc_id)
    auto tmp = head;
    for (int i = current_level - 1; i >= 0; --i) {
        while (tmp->forward_[i] && tmp->forward_[i]->key_ < key) {
            tmp = tmp->forward_[i];
        }
    }
    tmp = tmp->forward_[0];

    if (!tmp) {  // 空，表示要删除的元素不在
        return SkipListIterator();
    }

    // 情况1：未开启事务
    if (tranc_id == 0) {
        if (tmp->key_ == key) {
            return SkipListIterator(tmp);
        }
        return SkipListIterator();
    }

    // 情况2：开启事务（找 <= tranc_id 的版本）
    while (tmp && tmp->key_ == key) {
        if (tmp->tranc_id_ <= tranc_id) {
            return SkipListIterator(tmp);
        }
        tmp = tmp->forward_[0];
    }

    // 未找到返回空
    spdlog::trace("SkipList--get({}): not found", key);

    return SkipListIterator{};
}

// ! 这里的 remove 是跳表本身真实的 remove,  lsm 中通过使用 put 空值表示删除,
// ! 这里只是为了实现完整的 SkipList 不会真正被上层调用
void SkipList::remove(const std::string& key) {
    // TODO: 实现删除键值对
    // ? 从最高层开始查找目标节点并更新各层指针
    // ? 注意同时维护 backward_ 指针和 size_bytes

    // 步骤1：记录要更新的节点（即每层中要删除节点的前驱节点）
    std::vector<std::shared_ptr<SkipListNode>> update_nodes(max_level, nullptr); 
    auto tmp = head; 
    for(int i = current_level - 1; i >= 0; i--){
        // 依次遍历所有层，在第i层向右扫描，直到找到目标节点或遍历完
        while(tmp->forward_[i] && tmp->forward_[i]->key_ < key){
            tmp = tmp -> forward_[i];
        }
        update_nodes[i] = tmp;//每次记录插入位置的前节点
    }

    auto to_del_node = tmp->forward_[0];
    if(!to_del_node || to_del_node->key_ != key)
        return;

    for(int i = 0; i < current_level; i++){ //这个循环仅更新指针
        // 依据update_nodes中的记录进行更新(由于跳表的性质，删除时要修改的层数区间一定是连续的！)
        if(update_nodes[i]->forward_[i] != to_del_node){ break; }

        auto next = to_del_node->forward_[i];
        update_nodes[i]->forward_[i] = next;
        if(next){
            //如果删除节点不是末尾节点，说明要更新后面节点的前驱指针
            next->backward_[i] = update_nodes[i];
        }
    }
    // delet to_del_node;   智能指针自动释放

    // 更新跳表的内存大小
    size_bytes -= (key.size() + to_del_node->value_.size() + sizeof(uint64_t));
    // 步骤2：考虑跳表高度的下降
    while(current_level > 1 && head->forward_[current_level-1] == nullptr){
        current_level--;
    }
}

// 刷盘时调用，直接遍历最底层链表
std::vector<std::tuple<std::string, std::string, uint64_t>> SkipList::flush() {
    // std::shared_lock<std::shared_mutex> slock(rw_mutex);
    spdlog::debug("SkipList--flush(): Starting to flush skiplist data");

    std::vector<std::tuple<std::string, std::string, uint64_t>> data;
    auto node = head->forward_[0];
    while (node) {
        data.emplace_back(node->key_, node->value_, node->tranc_id_);
        node = node->forward_[0];
    }

    spdlog::debug("SkipList--flush(): Flushed {} entries", data.size());

    return data;
}

size_t SkipList::get_size() {
    // std::shared_lock<std::shared_mutex> slock(rw_mutex);
    return size_bytes;
}

// 清空跳表，释放内存
void SkipList::clear() {
    // std::unique_lock<std::shared_mutex> lock(rw_mutex);
    head = std::make_shared<SkipListNode>("", "", max_level, 0);
    size_bytes = 0;
}

// 返回跳表起始位置的迭代器
SkipListIterator SkipList::begin() {
    // return SkipListIterator(head->forward[0], rw_mutex);
    return SkipListIterator(head->forward_[0]);
}

// 返回跳表结束位置的迭代器
SkipListIterator SkipList::end() {
    return SkipListIterator();  // 使用空构造函数
}

// 找到前缀区间的起始位置的迭代器
// 返回第一个前缀匹配或者大于前缀的迭代器
SkipListIterator SkipList::begin_preffix(const std::string& preffix) {
    // TODO:  实现前缀查询的起始位置(y)
    // 实现跳表的查找+按字符前缀匹配
    spdlog::trace("SkipList--begin_preffix('{}') called", preffix);

    auto tmp = head;
    // 从最高层开始查找
    for (int i = current_level - 1; i >= 0; --i) {
        // 前缀匹配和查找本质上一样
        while (tmp->forward_[i] && tmp->forward_[i]->key_ < preffix) {
            tmp = tmp->forward_[i];
        }
    }
    tmp = tmp->forward_[0];  // 移动到最底层
    if (tmp && tmp->key_ == preffix) {
        spdlog::trace("SkipList--begin_preffix('{}'): first match at '{}'", preffix, tmp->key_);
    }
    return SkipListIterator(tmp);
}

// 找到前缀区间的末尾位置的迭代器
SkipListIterator SkipList::end_preffix(const std::string& prefix) {
    // TODO:  实现前缀查询的终结位置(y)
    // ? 找到第一个 key 不以 prefix 开头的节点作为终结位置
    // 如何快速查找？（目前依旧是跳表遍历）
    // 借助已有的起始位置迭代器？（这里不使用，感觉可以后续优化）
    // 如果借助起始位置迭代器，如果查找？（未提供向外的接口，感觉可以优化）
    spdlog::trace("SkipList--end_preffix('{}') called", prefix);

    auto tmp = head;
    // 从最高层开始查找
    for (int i = current_level - 1; i >= 0; --i) {
        while (tmp->forward_[i] && tmp->forward_[i]->key_ < prefix) {
            tmp = tmp->forward_[i];
        }
    }
    
    tmp = tmp->forward_[0];  // 移动到最底层
    // 找到第一个键不以给定前缀开头的节点
    while (tmp && tmp->key_.substr(0, prefix.size()) == prefix) {
        tmp = tmp->forward_[0];
    }

    if (tmp) {
        spdlog::trace("SkipList--begin_preffix('{}'): end at '{}'", prefix, tmp->key_);
    } else {
        spdlog::trace("SkipList--begin_preffix('{}'): end at the skiplist end", prefix);
    }
    // 返回当前节点的迭代器
    return SkipListIterator(tmp);
}

// ? 这里单调谓词的含义是, 整个数据库只会有一段连续区间满足此谓词
// ? 例如之前特化的前缀查询，以及后续可能的范围查询，都可以转化为谓词查询
// ? 返回第一个满足谓词的位置和最后一个满足谓词的迭代器
// ? 如果不存在, 返回 nullopt
// ? 谓词作用于key, 且保证满足谓词的结果只在一段连续的区间内, 例如前缀匹配的谓词
// ? predicate返回值:
// ?   0: 满足谓词
// ?   >0: 不满足谓词, 需要向右移动
// ?   <0: 不满足谓词, 需要向左移动
// ! Skiplist 中的谓词查询不会进行事务id的判断, 需要上层自己进行判断
std::optional<std::pair<SkipListIterator, SkipListIterator>> SkipList::iters_monotony_predicate(
    std::function<int(const std::string&)> predicate) {
    // ! 返回的是区间两端的迭代器
    // ? 分两步: 1. 利用多层跳表快速找到谓词满足区间内的一个节点
    // ?         2. 分别向前/向后扩展, 利用 backward_ 和 forward_确定区间边界
    // ? 注意: 向前查找时需要利用 backward_ 指针从当前节点的最高层开始回溯
    // 难道不是和普通查找一样？（不一样，谓词的大小逻辑可能正好和跳表的顺序相反！）
    auto current = head;
    SkipListIterator begin_iter = SkipListIterator(nullptr);
    SkipListIterator end_iter = SkipListIterator(nullptr);

    // 从最高层开始查找
    // 一开始 current == head, 所以  current_level - 1 处肯定有合法的指针
    bool find1 = false;
    for (int i = current_level - 1; i >= 0; --i) {
        while (!find1) {
            auto forward_i = current->forward_[i];
            if (forward_i == nullptr) {
                break;
            }
            auto direction = predicate(forward_i->key_);
            if (direction == 0) {
                // current 已经满足谓词了
                find1 = true;
                current = forward_i;
                break;
            } else if (direction < 0) {
                // 下一个位置不满足谓词, 且方向错误(位于目标区间右侧)
                // 需要尝试更小的步长(层级)
                break;
            } else {
                // 下一个位置不满足谓词, 但方向正确(位于目标区间左侧)
                current = forward_i;
            }
        }
    }

    if (!find1) {
        // 无法找到第一个满足谓词的迭代器, 直接返回
        spdlog::trace("SkipList--iters_monotony_predicate(): no match found");
        return std::nullopt;
    }

    // 记住当前 current 的位置
    auto current2 = current;

    // current 已经满足谓词, 需要前向检查找符合谓词条件的左边界
    // 注意此时 不能直接从 current_level - 1 层开始,
    // 因为当前节点的层数不一定等于最大层数
    for (int i = current->backward_.size() - 1; i >= 0; --i) {
        while (true) {
            if (current->backward_[i].lock() == nullptr ||
                current->backward_[i].lock() == head) {
                // 当前层没有前向节点, 或前向节点指向头结点
                break;
            }
            auto direction = predicate(current->backward_[i].lock()->key_);
            if (direction == 0) {
                // 前一个位置满足谓词, 继续判断
                current = current->backward_[i].lock();
                continue;
            } else if (direction > 0) {
                // 前一个位置不满足谓词，需要尝试更小的层级
                break;
            } else {
                // 因为当前位置满足了谓词, 前一个位置不可能返回-1
                // 这种情况属于跳表实现错误, 需要排查
                spdlog::error("iters_predicate: invalid direction");
                throw std::runtime_error("iters_predicate: invalid direction");
            }
        }
    }

    // 找到第一个满足谓词的节点
    begin_iter = SkipListIterator(current);

    // 找到满足谓词的条件的右边界区间
    for (int i = current2->forward_.size() - 1; i >= 0; --i) {
        while (true) {
            if (current2->forward_[i] == nullptr) {
                // 当前层没有后向节点
                break;
            }
            auto direction = predicate(current2->forward_[i]->key_);
            if (direction == 0) {
                // 后一个位置满足谓词, 继续判断
                current2 = current2->forward_[i];
                continue;
            } else if (direction < 0) {
                // 后一个位置不满足谓词，需要尝试更小的层级
                break;
            } else {
                // 因为当前位置满足了谓词, 后一个位置不可能返回1
                // 这种情况属于跳表实现错误, 需要排查
                spdlog::error("iters_predicate: invalid direction");
                throw std::runtime_error("iters_predicate: invalid direction");
            }
        }
    }

    end_iter = SkipListIterator(current2);
    // 转化为开区间
    ++end_iter;

    spdlog::trace("SkipList--iters_monotony_predicate(): range found");
    return std::make_optional<std::pair<SkipListIterator, SkipListIterator>>(begin_iter, end_iter);
}

// ? 打印跳表, 你可以在出错时调用此函数进行调试
void SkipList::print_skiplist() {
    for (int level = 0; level < current_level; level++) {
        std::cout << "Level " << level << ": ";
        auto current = head->forward_[level];
        while (current) {
            std::cout << current->key_;
            current = current->forward_[level];
            if (current) {
                std::cout << " -> ";
            }
        }
        std::cout << std::endl;
    }
    std::cout << std::endl;
}
}  // namespace tiny_lsm
