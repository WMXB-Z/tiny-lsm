#include "block/block_cache.h"

#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "block/block.h"

// LRU-K = 看“倒数第 K 次访问时间”（Kth most recent access time）
// 但工程上，LRU-K 里的 K = “成为热点所需的访问次数阈值”
// ✔ “访问 K 次” ⇔ “有第 K 次最近访问时间”，因为：
//     如果一个 key 访问 < K 次，没有 kth time（不存在）
//     如果访问 ≥ K 次，才能定义 kth most recent time

// 工程上的实现：分为两个队列
// 1）用 probationary 判断热点块（访问次数<k）
//     被访问后，通常位置可以不变（很多实现甚至不做移动）
//     可能只更新“访问次数 / history”
//     核心目标不是维护精确 LRU 顺序，只用于判断目标是否是热点数据
// 2）用 protected 用 LRU 近似维护热点块集合（访问次数>=k）
//    被访问后，一般会发生位置变化（通常 move-to-front）
//    按“LRU（最近1次访问）”移动
namespace tiny_lsm {
    
BlockCache::BlockCache(size_t capacity, size_t k) : capacity_(capacity), k_(k) {}

BlockCache::~BlockCache() = default;

std::shared_ptr<Block> BlockCache::get(int sst_id, int block_id) {
    // TODO: Lab 4.8 在缓存池中查询一个 Block
    std::lock_guard<std::mutex> lock(mutex_);
    ++total_requests_;  // 增加总请求数
    auto key = std::make_pair(sst_id, block_id);
    // find():得到迭代器std::unordered_map<std::pair<int, int>,
                    // std::list<CacheItem>::iterator>::iterator
    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) {
        return nullptr;  // 缓存未命中
    }

    ++hit_requests_;  // 增加命中请求数
    update_access_count(it->second);    // 更新访问次数
    return it->second->cache_block;
}

// put():
//   ├─ 判断缓存池容量是否到达上限，如果满了：
//   |  ├─ if 小于K的队列中有元素，则从中移除尾部元素
//   |  └─ else 从大于k的队列中尾部移除元素
//   └─ 放入新元素，更新控制信息map

void BlockCache::put(int sst_id, int block_id, std::shared_ptr<Block> block) {
    // TODO: Lab 4.8 在Block缓存池中插入一个 Block（已通过）
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = std::make_pair(sst_id, block_id);
    auto it = cache_map_.find(key);

    if (it != cache_map_.end()) {
        // 更新已有缓存项
        // ! 照理说 Block 类的数据是不可变的，这里的更新分支应该不会存在,
        // 只是debug用
        it->second->cache_block = block;
        update_access_count(it->second);
    } else {
        // 插入新缓存项
        if (cache_map_.size() >= capacity_) {
            // 移除最久未使用的缓存项
            if (!cache_list_less_k.empty()) {
                // 优先从 cache_list_less_k 中移除
                cache_map_.erase(std::make_pair(cache_list_less_k.back().sst_id,
                                   cache_list_less_k.back().block_id));
                cache_list_less_k.pop_back();
            } else {
                cache_map_.erase(std::make_pair(cache_list_greater_k.back().sst_id,
                                   cache_list_greater_k.back().block_id));
                cache_list_greater_k.pop_back();
            }
        }
        CacheItem item = {sst_id, block_id, block, 1};
        cache_list_less_k.push_front(item);
        // 放入映射表cache_map_中
        cache_map_[key] = cache_list_less_k.begin();
    }
}

// 计算命中率
double BlockCache::hit_rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_requests_ == 0
               ? 0.0
               : static_cast<double>(hit_requests_) / total_requests_;
}

void BlockCache::update_access_count(std::list<CacheItem>::iterator it) {
    // TODO: Lab 4.8 更新Block缓存池的统计信息（已通过）
    ++it->access_count;
    if (it->access_count < k_) {
        // 这表示更新后仍然位于cache_list_less_k，故只需要重新置于cache_list_less_k头部即可
        // splice():移动元素
        cache_list_less_k.splice(cache_list_less_k.begin(), cache_list_less_k, it);
    } else if (it->access_count == k_) {
        // 更新后满足k次访问, 升级链表
        // 从 cache_list_less_k 移动到 cache_list_greater_k 头部
        auto item = *it;
        cache_list_less_k.erase(it);
        cache_list_greater_k.push_front(item);
        cache_map_[std::make_pair(item.sst_id, item.block_id)] =
            cache_list_greater_k.begin();
    } else if (it->access_count > k_) {
        // 本来就位于 cache_list_greater_k
        // 移动到 cache_list_greater_k 头部
        cache_list_greater_k.splice(cache_list_greater_k.begin(),
                                    cache_list_greater_k, it);
    }
}
}  // namespace tiny_lsm