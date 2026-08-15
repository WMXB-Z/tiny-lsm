#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>

#include "iterator/iterator.h"
#include "skiplist/skiplist.h"

namespace tiny_lsm {

class BlockCache;
class SST;
class SSTBuilder;
class TranContext;

class MemTable {
    friend class TranContext;
    friend class HeapIterator;

private:
    void put_(const std::string& key, const std::string& value, uint64_t tranc_id);

    /**
     * @brief 按key查询内存表（底层调用cur_get_()、frozen_get_()）
     * @param key 
     * @param tranc_id 决定该key的可见性
     * @return SkipListIterator 
     */
    SkipListIterator get_(const std::string& key, uint64_t tranc_id);
    SkipListIterator cur_get_(const std::string& key, uint64_t tranc_id);
    SkipListIterator frozen_get_(const std::string& key, uint64_t tranc_id);

    void remove_(const std::string& key, uint64_t tranc_id);

    /**
     * @brief 将当前活表放入冻表集合（相当于转为冻表，新建一张空的活表）
     */
    void frozen_cur_table_();  // _ 表示不需要锁的版本

public:
    MemTable();
    ~MemTable() = default;

    void put(const std::string& key, const std::string& value, uint64_t tranc_id);
    void put_batch(const std::vector<std::pair<std::string, std::string>>& kvs, uint64_t tranc_id);

    SkipListIterator get(const std::string& key, uint64_t tranc_id);
    std::vector<std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>> get_batch(const std::vector<std::string>& keys, uint64_t tranc_id);
    void remove(const std::string& key, uint64_t tranc_id);
    void remove_batch(const std::vector<std::string>& keys, uint64_t tranc_id);

    void clear();

    /**
     * @brief 将最旧的冻表进行落盘，存放为level0中的SSTable
     * 
     * @param builder SSTable的构建器
     * @param sst_path SSTable的保存文件路径
     * @param sst_id 该SSTable的id
     * @param block_cache Block缓存池的指针
     * @return std::shared_ptr<SST> 
     */
    std::shared_ptr<SST> flush_last(SSTBuilder& builder, 
                                    std::string& sst_path,
                                    size_t sst_id,
                                    std::shared_ptr<BlockCache> block_cache);
    void frozen_cur_table();
    size_t get_cur_size();
    size_t get_frozen_size();
    size_t get_total_size();
    HeapIterator begin(uint64_t tranc_id);
    HeapIterator iters_preffix(const std::string& preffix, uint64_t tranc_id);

    std::optional<std::pair<HeapIterator, HeapIterator>>
    iters_monotony_predicate(uint64_t tranc_id, std::function<int(const std::string&)> predicate);

    HeapIterator end();

private:
    std::shared_ptr<SkipList> current_table;
    std::list<std::shared_ptr<SkipList>> frozen_tables; //处于内存中的冻表集合
    size_t frozen_bytes;
    std::shared_mutex frozen_mtx;  // 冻表的锁
    std::shared_mutex cur_mtx;     // 活表的锁
};
}  // namespace tiny_lsm