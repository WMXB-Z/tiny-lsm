#pragma once

#include <sys/types.h>

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "block.h"

namespace tiny_lsm {

// 定义缓存项
struct CacheItem {
    int sst_id;
    int block_id;
    std::shared_ptr<Block> cache_block; //data block块
    uint64_t access_count;  // 访问时间戳
};

// 自定义哈希函数，实现将(sst_id, block_id)映射为std::list<CacheItem>::iterator
struct pair_hash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
        auto hash1 = std::hash<T1>{}(p.first);
        auto hash2 = std::hash<T2>{}(p.second);
        return hash1 ^ hash2;
    }
};

// 自定义相等比较函数
struct pair_equal {
    template <class T1, class T2>
    bool operator()(const std::pair<T1, T2>& lhs, const std::pair<T1, T2>& rhs) const {
        return lhs.first == rhs.first && lhs.second == rhs.second;
    }
};

// 定义缓存池
class BlockCache {
public:
    BlockCache(size_t capacity, size_t k);
    ~BlockCache();

    // 获取缓存项中的std::shared_ptr<Block>
    std::shared_ptr<Block> get(int sst_id, int block_id);

    // 插入缓存项
    void put(int sst_id, int block_id, std::shared_ptr<Block> data);

    // 获取缓存命中率
    double hit_rate() const;

private:
    size_t capacity_;           // 缓存容量
    size_t k_;                  // LRU-K 中的 K 值
    mutable std::mutex mutex_;  // 互斥锁保护缓存池

    // 双向链表存储缓存项
    std::list<CacheItem> cache_list_greater_k;
    std::list<CacheItem> cache_list_less_k;

    // 哈希表索引缓存项(sst_id, block_id)映射为std::list<CacheItem>::iterator
    // map 用来 O(1) 查找
    // list 用来 O(1) 调整 LRU 顺序。
    // 通过找到的迭代器，.erase(迭代器)，能实现对List操作的O(1)
    std::unordered_map<std::pair<int, int>, std::list<CacheItem>::iterator,
                       pair_hash, pair_equal> cache_map_;

    // 更新缓存项的访问时间
    void update_access_count(std::list<CacheItem>::iterator it);

    // 记录请求数和命中数
    mutable size_t total_requests_ = 0;
    mutable size_t hit_requests_ = 0;
};
}  // namespace tiny_lsm