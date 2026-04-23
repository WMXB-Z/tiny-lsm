#include "memtable/memtable.h"

#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "config/config.h"
#include "consts.h"
#include "iterator/iterator.h"
#include "skiplist/skiplist.h"
#include "spdlog/spdlog.h"
#include "sst/sst.h"

// !LSM 的 iterator 本质不是“遍历某个结构”，而是把多个有序数据源 merge 成一个有序流：
// 1、SkipListIterator 负责访问“单表数据”，即提供数据来源
// 2、SearchItem 统一“跨表数据”形式，定义“排序规则”
// 3、HeapIterator 负责“多表归并”，按SearchItem规则排序+去重
// 4、MemTable 上述迭代器的上层调度者，负责：组织数据（cur_table,frozen_tables）、构建vector<SearchItem>、通过HeapIterator实现总体数据的访问

namespace tiny_lsm {

class BlockCache;
// MemTable implementation using PIMPL idiom
MemTable::MemTable() : frozen_bytes(0) {
    current_table = std::make_shared<SkipList>();
}
MemTable::~MemTable() = default;

void MemTable::put_(const std::string& key, const std::string& value, uint64_t tranc_id) {
    // TODO: Lab2.1 无锁版本的 put(个人代码已验证)
    // ? 直接调用 current_table 的 put 方法
    current_table->put(key, value, tranc_id);
}

void MemTable::put(const std::string& key, const std::string& value, uint64_t tranc_id) {
    // TODO: Lab2.1 有锁版本的 put(个人代码已验证)
    // ? 加 cur_mtx 写锁后调用 put_()
    // ? 若 current_table 超过 LsmPerMemSizeLimit, 还需加 frozen_mtx 写锁并调用 frozen_cur_table_()
    spdlog::trace("MemTable--put({}, {}, {}) called", key, value, tranc_id);

    std::unique_lock<std::shared_mutex> lock(cur_mtx);
    current_table->put(key, value, tranc_id);
    auto limit = TomlConfig::getInstance().getLsmPerMemSizeLimit();
    if(current_table->get_size() > limit){
        std::unique_lock<std::shared_mutex> lock(frozen_mtx);
       frozen_cur_table_();
    }

    spdlog::debug(
        "MemTable--Current table size exceeded limit. Frozen and "
        "created new table.");
}

void MemTable::put_batch(
    const std::vector<std::pair<std::string, std::string>>& kvs, uint64_t tranc_id) {
    // TODO: Lab2.1 有锁版本的 put_batch(个人代码已验证)
    // ? 加 cur_mtx 写锁后遍历 kvs 依次调用 put_()
    // ? 结束后若超限LsmPerMemSizeLimit则冻结当前表
    spdlog::trace("MemTable--put_batch with {} keys", kvs.size());

    std::unique_lock<std::shared_mutex> lock(cur_mtx);
    for(auto& kv: kvs){ 
        current_table->put(kv.first, kv.second, tranc_id); 
    }

    auto limit = TomlConfig::getInstance().getLsmPerMemSizeLimit();
    if(current_table->get_size() > limit){
        std::unique_lock<std::shared_mutex> lock(frozen_mtx);
        frozen_cur_table_(); 
        spdlog::debug(
            "MemTable--Current table size exceeded limit after batch "
            "put. Frozen and created new table.");
    }
}

SkipListIterator MemTable::cur_get_(const std::string& key, uint64_t tranc_id) {
    // 检查当前活跃的memtable
    // TODO: Lab2.1 从活跃跳表中查询(个人代码已验证)
    // ? 调用 current_table->get(), 找到则返回; 未找到则返回空迭代器
    // 检查当前活跃的memtable
    auto result = current_table->get(key, tranc_id);
    if (result.is_valid()) {
        // 只要找到了 key, 不管 value 是否为空都返回
        return result;
    }
    return SkipListIterator{};
}

SkipListIterator MemTable::frozen_get_(const std::string& key, uint64_t tranc_id) {
    // TODO: Lab2.1 从冻结跳表中查询(个人代码已验证)
    // ? 遍历 frozen_tables (注意顺序：越靠前越新), 找到即返回
    // ? tranc_id 直接传递到 get() 即可
    for(auto table : frozen_tables){
        auto res = table->get(key, tranc_id);
        if(res.is_valid()){
            return res;
        }
    }
    return SkipListIterator{};
}

SkipListIterator MemTable::get(const std::string& key, uint64_t tranc_id) {
    // TODO: Lab2.1 查询, 有锁版本，建议复用 cur_get_ 和 frozen_get_(个人代码已验证)
    // ? 先加 cur_mtx 读锁查活跃表, 未命中则释放锁后加 frozen_mtx 读锁查冻结表
    spdlog::trace("MemTable--get({}) called", key);
    // 先获取当前活跃表的锁
    std::shared_lock<std::shared_mutex> slock1(cur_mtx);
    auto cur_res = cur_get_(key, tranc_id);
    if (cur_res.is_valid()) {
        return cur_res;
    }
    // 活跃表没有找到，再获取冻结表的锁
    slock1.unlock();
    std::shared_lock<std::shared_mutex> slock2(frozen_mtx);
    auto frozen_result = frozen_get_(key, tranc_id);
    if (frozen_result.is_valid()) {
        return frozen_result;
    }

    spdlog::trace("MemTable--get({}): key not found", key);
    return SkipListIterator{};

}

SkipListIterator MemTable::get_(const std::string& key, uint64_t tranc_id) {
    // TODO: Lab2.1 查询, 无锁版本(个人代码已验证)
    // ? 直接调用 cur_get_ 和 frozen_get_
    spdlog::trace("MemTable--get_({}) called", key);
    auto reslut =  cur_get_(key, tranc_id);
    if(reslut.is_valid()){
        return reslut;
    }
    reslut = frozen_get_(key, tranc_id);
    if (reslut.is_valid()) {
        return reslut;
    }
    spdlog::trace("MemTable--get_({}): key not found", key);
    return SkipListIterator{};
}

std::vector<
    std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
MemTable::get_batch(const std::vector<std::string>& keys, uint64_t tranc_id) {
    spdlog::trace("MemTable--get_batch with {} keys", keys.size());

    std::vector<
        std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
        results;
    results.reserve(keys.size());

    // 1. 先获取活跃表的锁
    std::shared_lock<std::shared_mutex> slock1(cur_mtx);
    for (size_t idx = 0; idx < keys.size(); idx++) {
        auto key = keys[idx];
        auto cur_res = cur_get_(key, tranc_id);
        if (cur_res.is_valid()) { // 值存在且不为空
            results.emplace_back(key, std::make_pair(cur_res.get_value(),
                                                     cur_res.get_tranc_id()));
        } else {
            // 如果活跃表中未找到，先占位
            results.emplace_back(key, std::nullopt);
        }
    }

    // 2. 如果某些键在活跃表中未找到，还需要查找冻结表
    if (!std::any_of(results.begin(), results.end(), [](const auto& result) {
            return !result.second.has_value();
        })) {
        return results;
    }

    slock1.unlock();                                         // 释放活跃表的锁
    std::shared_lock<std::shared_mutex> slock2(frozen_mtx);  // 获取冻结表的锁
    for (size_t idx = 0; idx < keys.size(); idx++) {
        if (results[idx].second.has_value()) {
            continue;  // 如果在活跃表中已经找到，则跳过
        }
        auto key = keys[idx];
        auto frozen_result = frozen_get_(key, tranc_id);
        if (frozen_result.is_valid()) {
            // 值存在且不为空
            results[idx] = std::make_pair(
                key, std::make_pair(frozen_result.get_value(),
                                    frozen_result.get_tranc_id()));
        } else {
            results[idx] = std::make_pair(key, std::nullopt);
        }
    }

    return results;
}

void MemTable::remove_(const std::string& key, uint64_t tranc_id) {
    // TODO: Lab2.1 无锁版本的remove(个人代码已验证)
    // ? 在 LSM 中, 删除操作是写入空值, 调用 current_table->put(key, "", tranc_id)
    spdlog::trace("MemTable--remove_({}) called", key);
    current_table->put(key, "", tranc_id);
}

void MemTable::remove(const std::string& key, uint64_t tranc_id) {
    // TODO: Lab2.1 有锁版本的remove(个人代码已验证)
    // ? 加 cur_mtx 写锁后调用 remove_()
    // ? 若超限则冻结当前表
    // 为什么删除也会发生表的冻结？
    spdlog::trace("MemTable--remove({}) called", key);

    std::unique_lock<std::shared_mutex> lock(cur_mtx);
    remove_(key, tranc_id);
    if (current_table->get_size() > TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
        // 冻结当前表还需要获取frozen_mtx的写锁
        std::unique_lock<std::shared_mutex> lock2(frozen_mtx);
        frozen_cur_table_();
        spdlog::debug(
            "MemTable--Current table size exceeded limit after remove. "
            "Frozen and created new table.");
    }
}

void MemTable::remove_batch(const std::vector<std::string>& keys, uint64_t tranc_id) {
    // TODO: Lab2.1 有锁版本的remove_batch(个人代码已验证)
    // ? 加 cur_mtx 写锁后遍历 keys 依次调用 remove_()
    // ? 结束后若超限则冻结当前表
    std::unique_lock<std::shared_mutex> lock(cur_mtx);
    // 删除的方式是写入空值
    for (auto& key : keys) {
        remove_(key, tranc_id);
    }
    if (current_table->get_size() > TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
        // 冻结当前表还需要获取frozen_mtx的写锁
        std::unique_lock<std::shared_mutex> lock2(frozen_mtx);
        frozen_cur_table_();
    }
}

void MemTable::clear() {
    spdlog::info("MemTable--clear(): Clearing all tables");

    std::unique_lock<std::shared_mutex> lock1(cur_mtx);
    std::unique_lock<std::shared_mutex> lock2(frozen_mtx);
    frozen_tables.clear();
    current_table->clear();
}

// 超出阈值后，将 memtable中最老的frozen_table 写为一个 SST, 并返回SST的指针
// 而fozen_table各表间的数据虽然存在重复+区间重叠，但每张frozen_table中的数据是区间有序的（仍可能有key重复）
std::shared_ptr<SST> MemTable::flush_last(
    SSTBuilder& builder, std::string& sst_path, size_t sst_id,
    std::shared_ptr<BlockCache> block_cache) {

    spdlog::debug("MemTable--flush_last(): Starting to flush memtable to SST{}", sst_id);

    // 由于 flush 后需要移除最老的 memtable, 因此需要加写锁
    std::unique_lock<std::shared_mutex> lock(frozen_mtx);

    uint64_t max_tranc_id = 0;
    uint64_t min_tranc_id = UINT64_MAX;

    if (frozen_tables.empty()) {
        // 如果当前表为空，直接返回nullptr
        if (current_table->get_size() == 0) {
            spdlog::debug(
                "MemTable--flush_last(): Current table is empty, returning "
                "null");

            return nullptr;
        }
        // 将当前表加入到frozen_tables头部
        frozen_tables.push_front(current_table);
        frozen_bytes += current_table->get_size();
        // 创建新的空表作为当前表
        current_table = std::make_shared<SkipList>();
    }

    // 将memtable中最老的frozen_tables取出写入 SST
    std::shared_ptr<SkipList> table = frozen_tables.back();
    frozen_tables.pop_back();
    frozen_bytes -= table->get_size();

    auto flush_data = table->flush();
    for (auto& [k, v, t] : flush_data) {
        max_tranc_id = (std::max)(t, max_tranc_id);
        min_tranc_id = (std::min)(t, min_tranc_id);
        builder.add(k, v, t);
    }
    auto sst = builder.build(sst_id, sst_path, block_cache);

    spdlog::info("MemTable--flush_last(): SST{} built successfully at '{}'", sst_id, sst_path);

    return sst;
}

void MemTable::frozen_cur_table_() {
    // TODO: Lab2.1 冻结活跃表（无锁版本）(个人代码已验证)
    // ? 将 current_table 移入 frozen_tables 头部, 并更新 frozen_bytes
    // ? 创建新的空 SkipList 作为 current_table
    spdlog::trace("MemTable--frozen_cur_table_(): Freezing current table");

    frozen_bytes += current_table->get_size();
    frozen_tables.push_front(std::move(current_table)); //move的情况下，emplace和push性能差不多
    current_table = std::make_shared<SkipList>();   //新建一张活动表
}

void MemTable::frozen_cur_table() {
    // TODO: Lab2.1 冻结活跃表（有锁版本）(个人代码已验证)
    // ? 加 cur_mtx 和 frozen_mtx 写锁后调用 frozen_cur_table_()
    spdlog::trace(
        "MemTable--frozen_cur_table(): Acquiring locks and freezing "
        "current table");

    std::shared_lock<std::shared_mutex> lock1(cur_mtx);
    std::shared_lock<std::shared_mutex> lock2(frozen_mtx);
    frozen_cur_table_();
}

size_t MemTable::get_cur_size() {
    std::shared_lock<std::shared_mutex> slock(cur_mtx);
    return current_table->get_size();
}

size_t MemTable::get_frozen_size() {
    std::shared_lock<std::shared_mutex> slock(frozen_mtx);
    return frozen_bytes;
}

size_t MemTable::get_total_size() {
    std::shared_lock<std::shared_mutex> slock1(cur_mtx);
    std::shared_lock<std::shared_mutex> slock2(frozen_mtx);
    // return get_frozen_size() + get_cur_size();
     return frozen_bytes + current_table->get_size();
}

// TODO: 需要进一步判断这里的 HeapIterator 能否跳过删除元素
HeapIterator MemTable::begin(uint64_t tranc_id) {
    // TODO: Lab2.2 MemTable 的迭代器
    // ? 加 cur_mtx 和 frozen_mtx 读锁, 遍历所有表收集 SearchItem
    // ? 每个 item 包含 key, value, table_idx, 0, tranc_id
    // ? 过滤 tranc_id 不可见的记录 (tranc_id != 0 && iter.get_tranc_id() > tranc_id) ? 返回 HeapIterator(item_vec, tranc_id)
    // 对current_table、frozens_tables的访问需要互斥，保证构建迭代器过程中数据不可变动
    // 虽然frozens_table的是不可变的，但作为存放这些表的容器数组frozen_tables是可变的。
    std::shared_lock<std::shared_mutex> slock1(cur_mtx);
    std::shared_lock<std::shared_mutex> slock2(frozen_mtx);
    std::vector<SearchItem> item_vec;

    for (auto iter=current_table->begin(); iter != current_table->end(); ++iter) {
        if (tranc_id != 0 && iter.get_tranc_id() > tranc_id) {
            continue;
        }
        item_vec.emplace_back(iter.get_key(), iter.get_value(), 0, 0, iter.get_tranc_id());
    }

    int table_idx = 1;
    for (auto ft = frozen_tables.begin(); ft != frozen_tables.end(); ft++) {
        auto table = *ft;
        for (auto iter = table->begin(); iter != table->end(); ++iter) {
            if (tranc_id != 0 && iter.get_tranc_id() > tranc_id) {
                continue;
            }
            item_vec.emplace_back(iter.get_key(), iter.get_value(), table_idx, 0, iter.get_tranc_id());
        }
        table_idx++;
    }
    return HeapIterator(item_vec, tranc_id);
}

HeapIterator MemTable::end() {
    // TODO: Lab2.2 MemTable 的迭代器
    // ? 加读锁后返回空 HeapIterator
    std::shared_lock<std::shared_mutex> slock1(cur_mtx);
    std::shared_lock<std::shared_mutex> slock2(frozen_mtx);
    // 没有显示定义无参构造器：编译器帮你生成了“隐式构造 + NRVO优化”，直接在返回位置构造（RVO），不需要拷贝/移动
    // 显式写了 = default,编译器可能不再应用优化,就必须真的调用拷贝/移动构造,于是报错
    return HeapIterator{};
}


HeapIterator MemTable::iters_preffix(const std::string& preffix, uint64_t tranc_id) {
    // TODO: Lab2.3 MemTable 的前缀迭代器（已通过）
    // ? 加读锁, 对所有表调用 begin_preffix/end_preffix 遍历前缀范围
    // ? 过滤事务可见性, 同 key 只保留最新版本
    spdlog::trace("MemTable--iters_preffix('{}', tranc_id={})", preffix, tranc_id);

    std::shared_lock<std::shared_mutex> slock1(cur_mtx);
    std::shared_lock<std::shared_mutex> slock2(frozen_mtx);
    std::vector<SearchItem> item_vec;

    // 遍历curren_table，查询符合前缀要求的元素
    for (auto iter = current_table->begin_preffix(preffix); iter != current_table->end_preffix(preffix); ++iter) {
        if (tranc_id != 0 && iter.get_tranc_id() > tranc_id) {
            // 如果开启了事务, 比当前事务 id 更大的记录是不可见的
            continue;
        }
        if (!item_vec.empty() && item_vec.back().key_ == iter.get_key()) {
            // 如果key相同，则只保留最新的事务修改的记录即可
            // 且这个记录既然已经存在于item_vec中，则其肯定满足了事务的可见性判断
            continue;
        }
        item_vec.emplace_back(iter.get_key(), iter.get_value(), 0, 0, iter.get_tranc_id());

        spdlog::trace("MemTable--iters_preffix(): get range from curent table");
    }

    // 遍历各个frozen_table，查询符合前缀要求的元素
    int table_idx = 1;
    for (auto ft = frozen_tables.begin(); ft != frozen_tables.end(); ft++) {
        auto table = *ft;
        for (auto iter = table->begin_preffix(preffix);
             iter != table->end_preffix(preffix); ++iter) {
            if (tranc_id != 0 && iter.get_tranc_id() > tranc_id) {
                // 如果开启了事务, 比当前事务 id 更大的记录是不可见的
                continue;
            }
            if (!item_vec.empty() && item_vec.back().key_ == iter.get_key()) {
                // 如果key相同，则只保留最新的事务修改的记录即可
                // 且这个记录既然已经存在于item_vec中，则其肯定满足了事务的可见性判断
                continue;
            }
            item_vec.emplace_back(iter.get_key(), iter.get_value(), table_idx,
                                  0, iter.get_tranc_id());

            spdlog::trace("MemTable--iters_preffix(): get range from table{}",
                          table_idx);
        }
        table_idx++;
    }
    return HeapIterator(item_vec, tranc_id);
}

std::optional<std::pair<HeapIterator, HeapIterator>>
MemTable::iters_monotony_predicate(uint64_t tranc_id, std::function<int(const std::string&)> predicate) {
    // TODO: Lab2.3 MemTable 的谓词查询迭代器起始范围（已通过）
    // ? 加读锁, 对所有表调用 iters_monotony_predicate 获取结果
    // ? 过滤事务可见性, 同 key 只保留最新版本
    // ? 若结果为空返回 nullopt; 否则返回 make_pair(HeapIterator(item_vec, tranc_id, true), HeapIterator{})
    // 该层面的查找是对curent_table和各个frozen_table进行谓词区间查找（利用SkipList上的谓词查找接口）
    // 然后把各个区间中，符合事务要求的结果汇总到item_vec，并返回相应的结果。
    // !注意：返回的结果区间中，仍存在重复key元素（同key的新旧版本），去重操作推迟至真正访问元素的时候才会进行（重载了operator++）
    spdlog::trace("MemTable--iters_monotony_predicate(tranc_id={}) called", tranc_id);

    std::shared_lock<std::shared_mutex> slock1(cur_mtx);
    std::shared_lock<std::shared_mutex> slock2(frozen_mtx);

    std::vector<SearchItem> item_vec;

    auto cur_result = current_table->iters_monotony_predicate(predicate);
    if (cur_result.has_value()) {
        auto [begin, end] = cur_result.value();
        for (auto iter = begin; iter != end; ++iter) {
            if (tranc_id != 0 && iter.get_tranc_id() > tranc_id) {
                // 如果开启了事务, 比当前事务 id 更大的记录是不可见的
                continue;
            }
            if (!item_vec.empty() && item_vec.back().key_ == iter.get_key()) {
                // 如果key相同，则只保留最新的事务修改的记录即可
                // 且这个记录既然已经存在于item_vec中，则其肯定满足了事务的可见性判断
                continue;
            }
            item_vec.emplace_back(iter.get_key(), iter.get_value(), 0, 0, iter.get_tranc_id());

            spdlog::trace("MemTable--iters_monotony_predicate(): get range from curent table");
        }
    }

    int table_idx = 1;
    for (auto ft = frozen_tables.begin(); ft != frozen_tables.end(); ft++) {
        auto table = *ft;
        auto result = table->iters_monotony_predicate(predicate);
        if (result.has_value()) {
            auto [begin, end] = result.value();
            for (auto iter = begin; iter != end; ++iter) {
                if (tranc_id != 0 && iter.get_tranc_id() > tranc_id) {
                    // 如果开启了事务, 比当前事务 id 更大的记录是不可见的
                    continue;
                }
                if (!item_vec.empty() && item_vec.back().key_ == iter.get_key()) {
                    // 如果key相同，则只保留最新的事务修改的记录即可
                    // 且这个记录既然已经存在于item_vec中，则其肯定满足了事务的可见性判断
                    continue;
                }
                item_vec.emplace_back(iter.get_key(), iter.get_value(), table_idx, 0, iter.get_tranc_id());
            }

            spdlog::trace("MemTable--iters_monotony_predicate(): get range from table{}", table_idx);
        }
        table_idx++;
    }

    if (item_vec.empty()) {
        spdlog::trace( "MemTable--iters_monotony_predicate(): No matching keys found");
        return std::nullopt;
    }
    return std::make_pair(HeapIterator(item_vec, tranc_id, true), HeapIterator{});
}
}  // namespace tiny_lsm
