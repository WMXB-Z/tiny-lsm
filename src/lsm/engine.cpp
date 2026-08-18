#include "lsm/engine.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "config/config.h"
#include "logger/logger.h"
#include "lsm/level_iterator.h"
#include "spdlog/spdlog.h"
#include "sst/concact_iterator.h"
#include "sst/sst.h"
#include "sst/sst_iterator.h"

namespace tiny_lsm {

// *********************** LSMEngine ***********************
LSMEngine::LSMEngine(std::string path) : data_dir(path) {
    // TODO:引擎初始化
    // ? 1. 初始化日志: init_spdlog_file()
    // ? 2. 初始化 block_cache (容量和 K 值从 TomlConfig 读取)
    // ? 3. 若目录不存在则创建
    // ? 4. 初始化 VLog: vlog_ = VLog::open(data_dir + "/vlog.data")，这是用于键值分离
    // ? 5. 遍历目录加载所有已存在的 SST 文件:
    // ?    - 文件名格式: sst_{id}.{level}
    // ?    - 调用 SST::open 并记录到 ssts 和level_sst_ids
    // ?    - 维护 next_sst_id 和 cur_max_level
    // ? 6. next_sst_id自增
    // ? 7. 对各层 sst_id_list 排序; L0 层需要 reverse (越大的 id 越新,优先查询) 初始化日志
    init_spdlog_file();

    // 初始化 block_cahce
    block_cache = std::make_shared<BlockCache>(TomlConfig::getInstance().getLsmBlockCacheCapacity(),
                                               TomlConfig::getInstance().getLsmBlockCacheK());

    // 创建数据目录
    if (!std::filesystem::exists(path)) {
        spdlog::info(
            "LSMEngine--"
            "DB path ndo not exist. Creating data directory: {}",
            path);
        std::filesystem::create_directory(path);
    }

    // 初始化 VLog (总是open, 这是为了保证WiscKey
    // 阈值为0，也能保证文件存在，从而正常重启读取)
    vlog_ = VLog::open(data_dir + "/vlog.data");

    if (std::filesystem::exists(path)) {
        // 如果目录存在，则检查是否有 sst 文件并加载
        spdlog::info(
            "LSMEngine--"
            "DB path exist. Loading data directory: {} ...",
            path);

        for (const auto &entry : std::filesystem::directory_iterator(path)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            std::string filename = entry.path().filename().string();
            // SST文件名格式为: sst_{id}.level
            if (!filename.starts_with("sst_")) {
                continue;
            }

            // 找到 . 的位置
            size_t dot_pos = filename.find('.');
            if (dot_pos == std::string::npos || dot_pos == filename.length() - 1) {
                continue;
            }

            // 提取 level
            std::string level_str = filename.substr(dot_pos + 1, filename.length() - 1 - dot_pos);
            if (level_str.empty()) {
                continue;
            }
            size_t level = std::stoull(level_str);

            // 提取SST ID
            std::string id_str = filename.substr(4, dot_pos - 4);  // 4 for "sst_"
            if (id_str.empty()) {
                continue;
            }
            size_t sst_id = std::stoull(id_str);

            // 加载SST文件, 初始化时需要加写锁
            std::unique_lock<std::shared_mutex> lock(ssts_mtx);  // 写锁
            next_sst_id = (std::max)(sst_id, next_sst_id);       // 记录目前最大的 sst_id
            cur_max_level = (std::max)(level, cur_max_level);    // 记录目前最大的 level
            std::string sst_path = get_sst_path(sst_id, level);
            auto sst = SST::open(sst_id, FileObj::open(sst_path, false), block_cache, vlog_);
            spdlog::info("LSMEngine-- Loaded SST: {} successfully!", sst_path);
            ssts[sst_id] = sst;

            level_sst_ids[level].push_back(sst_id);
        }

        next_sst_id++;  // 现有的最大 sst_id 自增后才是下一个分配的 sst_id

        for (auto &[level, sst_id_list] : level_sst_ids) {
            // 对各层 sst_id_list
            // 排序以便方便后续查询，排序要求是sst_id大的在前面
            std::sort(sst_id_list.begin(), sst_id_list.end());
            if (level == 0) {
                // 非 level-0 的 sst之间 在区间上的无重叠, 故不需要 reverse
                // 由于非level-0的 sst 整体有序，故查找元素时可以直接二分查询
                std::reverse(sst_id_list.begin(), sst_id_list.end());
            }
        }
    }
}

LSMEngine::~LSMEngine() = default;

std::optional<std::pair<std::string, uint64_t>> LSMEngine::get(const std::string &key, uint64_t tranc_id) {
    // TODO: 查询
    // ? 1. 先查 memtable.get(key, tranc_id), 命中则返回 (value 非空) 或 nullopt
    // (value 为空=删除) ? 2. 加 ssts_mtx 读锁, 遍历 L0 的 sst_ids (越大越新),
    // 通过 sst->get() 查询 ? 3. 遍历 L1 及以上各层, 对每层做二分查找确定
    // key所在的 SST 文件 ? 注意: value 为空字符串表示 key 已被删除,
    // 此时返回nullopt

    // 1. 先查找 memtable
    auto mem_res = memtable.get(key, tranc_id);
    if (mem_res.is_valid()) {
        if (mem_res.get_value().size() > 0) {
            // 值存在且不为空（没有被删除）
            spdlog::trace(
                "LSMEngine--"
                "get({},{}): value = {}, tranc_id = {} "
                "returning from memtable",
                key, tranc_id, mem_res.get_value(), mem_res.get_tranc_id());
            return std::pair<std::string, uint64_t>{mem_res.get_value(), mem_res.get_tranc_id()};
        } else {
            // memtable返回的kv的value为空值表示被删除了
            spdlog::trace(
                "LSMEngine--"
                "get({},{}): key is deleted, returning "
                "from memtable",
                key, tranc_id);
            return std::nullopt;
        }
    }

    // 2. level-0的sst中查询
    std::shared_lock<std::shared_mutex> rlock(ssts_mtx);  // 读锁
    for (auto &sst_id : level_sst_ids[0]) {
        //  level-0中的 sst_id 是按从大到小的顺序排列, sst_id 越大,
        //  表示是越晚刷入的, 优先查询
        auto &sst = ssts[sst_id];
        auto sst_iterator = sst->get(key, tranc_id);
        if (sst_iterator != sst->end()) {
            if ((sst_iterator)->second.size() > 0) {
                // 值存在且不为空（没有被删除）
                spdlog::trace(
                    "LSMEngine--"
                    "get({},{}): value = {}, tranc_id = {} "
                    "returning from l0 sst{}",
                    key, tranc_id, sst_iterator->second, sst_iterator.get_tranc_id(), sst_id);
                return std::pair<std::string, uint64_t>{sst_iterator->second, sst_iterator.get_tranc_id()};
            } else {  // 空值表示被删除了
                spdlog::trace(
                    "LSMEngine--"
                    "get({},{}): key is deleted or do not "
                    "exist , returning "
                    "from l0 sst{}",
                    key, tranc_id, sst_id);
                return std::nullopt;
            }
        }
    }

    // 3. level1+的sst中查询
    for (size_t level = 1; level <= cur_max_level; level++) {
        std::deque<size_t> l_sst_ids = level_sst_ids[level];
        // 二分查询
        size_t left = 0;
        size_t right = l_sst_ids.size();
        while (left < right) {  // 对sst_id做二分查找，相当于对key组间的二分检索
            size_t mid = left + (right - left) / 2;
            auto &sst = ssts[l_sst_ids[mid]];
            if (sst->get_first_key() <= key && key <= sst->get_last_key()) {
                // 如果sst_id在中, 则在sst中查询
                auto sst_iterator = sst->get(key, tranc_id);
                if (sst_iterator.is_valid()) {
                    if ((sst_iterator)->second.size() > 0) {
                        // 值存在且不为空（没有被删除）
                        spdlog::trace(
                            "LSMEngine--"
                            "get({},{}): value = {}, tranc_id = {} "
                            "returning from l{} sst{}",
                            key, tranc_id, sst_iterator->second, sst_iterator.get_tranc_id(), level, l_sst_ids[mid]);

                        return std::pair<std::string, uint64_t>{sst_iterator->second, sst_iterator.get_tranc_id()};
                    } else {
                        // 空值表示被删除了
                        spdlog::trace(
                            "LSMEngine--"
                            "get({},{}): key is deleted or do not exist "
                            "returning from l{} sst{}",
                            key, tranc_id, level, l_sst_ids[mid]);

                        return std::nullopt;
                    }
                } else {
                    break;
                }
            } else if (sst->get_last_key() < key) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
    }

    spdlog::trace(
        "LSMEngine--"
        "get({},{}): key is not exist, returning "
        "after checking all ssts",
        key, tranc_id);

    return std::nullopt;
}

std::vector<std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>> LSMEngine::get_batch(
    const std::vector<std::string> &keys, uint64_t tranc_id) {
    // TODO: 批量查询
    // ? 1. 先从 memtable 批量查询: memtable.get_batch(keys, tranc_id)
    // ? 2. 若有未命中项, 加读锁后依次查 L0 各 SST 文件
    // ? 3. 若仍有未命中, 对各高层 SST 做二分查找补全结果

    // 1. 先从 memtable 中批量key查找
    auto results = memtable.get_batch(keys, tranc_id);
    // 2. 如果所有键都在memtable 中找到，直接返回
    bool need_search_sst = false;
    for (const auto &[key, value] : results) {
        if (!value.has_value()) {
            // 需要查找（一组key中，只要有一个key没找到就要去sst中再尝试查找）
            need_search_sst = true;
            break;
        }
    }

    if (!need_search_sst) {
        return results;  // 不需要查sst
    }

    // 2. 从 L0 层 SST 文件中批量查找未命中的键
    std::shared_lock<std::shared_mutex> rlock(ssts_mtx);  // 加读锁

    for (auto &[key, value] : results) {
        if (value.has_value()) {
            continue;
        }  // 已找到，跳过
        for (auto &sst_id : level_sst_ids[0]) {
            auto &sst = ssts[sst_id];
            auto sst_iterator = sst->get(key, tranc_id);  // sst中单key查询

            if (sst_iterator != sst->end()) {
                if (sst_iterator->second.size() > 0) {
                    // 值存在且不为空
                    value = std::make_pair(sst_iterator->second, sst_iterator.get_tranc_id());
                } else {
                    // 空值表示被删除
                    value = std::nullopt;
                }
                break;  // 这张sst表中没有，停止查找，进入下一张sst表
            }
        }
    }

    // 3. 从其他层级 SST 文件中批量查找未命中的键
    for (size_t level = 1; level <= cur_max_level; level++) {
        std::deque<size_t> l_sst_ids = level_sst_ids[level];

        for (auto &[key, value] : results) {
            if (value.has_value()) {
                continue;
            }  // 已找到，跳过

            // 二分查找确定键可能所在的 SST 文件
            size_t left = 0;
            size_t right = l_sst_ids.size();
            while (left < right) {
                size_t mid = left + (right - left) / 2;
                auto &sst = ssts[l_sst_ids[mid]];

                if (sst->get_first_key() <= key && key <= sst->get_last_key()) {
                    // 如果键在当前 SST 文件范围内，则在 SST 中查找
                    auto sst_iterator = sst->get(key, tranc_id);
                    if (sst_iterator.is_valid()) {
                        if (sst_iterator->second.size() > 0) {
                            // 值存在且不为空
                            value = std::make_pair(sst_iterator->second, sst_iterator.get_tranc_id());
                        } else {
                            // 空值表示被删除
                            value = std::nullopt;
                        }
                    }
                    break;  // 这张sst表中没有，停止查找，进入下一张sst表
                } else if (sst->get_last_key() < key) {
                    left = mid + 1;
                } else {
                    right = mid;
                }
            }
        }
    }

    return results;
}

std::optional<std::pair<std::string, uint64_t>> LSMEngine::sst_get_(const std::string &key, uint64_t tranc_id) {
    // TODO: sst 内部查询，不加锁
    // (专门在SST中进行查询的接口，不查memtable)（已通过） ? 逻辑与 get() 的 SST
    // 部分相同, 先 L0 后 L1+
    // 1. level-0 sst中查询
    for (auto &sst_id : level_sst_ids[0]) {
        //  中的 sst_id 是按从大到小的顺序排列,
        // sst_id 越大, 表示是越晚刷入的, 优先查询
        auto sst = ssts[sst_id];
        auto sst_iterator = sst->get(key, tranc_id);
        if (sst_iterator != sst->end()) {
            if ((sst_iterator)->second.size() > 0) {
                // 值存在且不为空（没有被删除）
                // L0 SST 查询命中
                spdlog::trace(
                    "LSMEngine--"
                    "sst_get({}{}): found in l0 sst{}",
                    key, tranc_id, sst_id);

                return std::pair<std::string, uint64_t>{sst_iterator->second, sst_iterator.get_tranc_id()};
            } else {
                // 空值表示被删除了
                return std::nullopt;
            }
        }
    }

    // 2. 其他level的sst中查询
    for (size_t level = 1; level <= cur_max_level; level++) {
        std::deque<size_t> l_sst_ids = level_sst_ids[level];
        // 二分查询
        size_t left = 0;
        size_t right = l_sst_ids.size();
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            auto sst = ssts[l_sst_ids[mid]];
            if (sst->get_first_key() <= key && key <= sst->get_last_key()) {
                // 如果sst_id在中, 则在sst中查询
                auto sst_iterator = sst->get(key, tranc_id);
                if (sst_iterator.is_valid()) {
                    if ((sst_iterator)->second.size() > 0) {
                        // 值存在且不为空（没有被删除）
                        // 其他 Level SST 查询命中
                        spdlog::trace(
                            "LSMEngine--"
                            "sst_get({}{}): found in l{} sst{}",
                            key, tranc_id, level, sst_iterator.get_tranc_id());

                        return std::pair<std::string, uint64_t>{sst_iterator->second, sst_iterator.get_tranc_id()};
                    } else {
                        // 空值表示被删除了
                        return std::nullopt;
                    }
                } else {
                    break;
                }
            } else if (sst->get_last_key() < key) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
    }

    spdlog::trace(
        "LSMEngine--"
        "sst_get({}{}): key is not exist, returning "
        "after checking all ssts",
        key, tranc_id);

    return std::nullopt;
}

uint64_t LSMEngine::put(const std::string &key, const std::string &value, uint64_t tranc_id) {
    // TODO: 插入
    // ? 调用 memtable.put(key, value, tranc_id)
    // ? 若 memtable 总大小 >= LsmTolMemSizeLimit 则调用 flush()
    // 并返回其结果(结果是新的最大事务Id) ? 否则返回 0
    memtable.put(key, value, tranc_id);
    spdlog::trace("LSMEngine--put({}, {}, {}) inserted into memtable", key, value, tranc_id);

    // 如果 memtable 太大，需要刷新到磁盘
    // std::cout << "memtable的大小：" << memtable.get_total_size() << '\n';
    // std::cout << "MemSizeLimit大小：" <<
    // TomlConfig::getInstance().getLsmTolMemSizeLimit() << "\n";
    if (memtable.get_total_size() >= TomlConfig::getInstance().getLsmTolMemSizeLimit()) {
        return flush();
    }
    return 0;
}

uint64_t LSMEngine::put_batch(const std::vector<std::pair<std::string, std::string>> &kvs, uint64_t tranc_id) {
    // TODO: 批量插入
    // ? 调用 memtable.put_batch(kvs, tranc_id)
    // ? 若超限则 flush() 并返回其结果
    memtable.put_batch(kvs, tranc_id);
    spdlog::trace(
        "LSMEngine--"
        "put_batch with {} keys inserted into memtable",
        kvs.size());

    // 如果 memtable 太大，需要刷新到磁盘
    if (memtable.get_total_size() >= TomlConfig::getInstance().getLsmTolMemSizeLimit()) {
        return flush();
    }
    return 0;
}

uint64_t LSMEngine::remove(const std::string &key, uint64_t tranc_id) {
    // TODO: 删除
    // ? 在 LSM 中，删除实际上是插入一个空值
    // ? 调用 memtable.remove(key, tranc_id)
    // ? 若超限则 flush() 并返回其结果
    memtable.remove(key, tranc_id);

    spdlog::trace("LSMEngine--remove({}, {}) marked as deleted in memtable", key, tranc_id);

    // 如果 memtable 太大，需要刷新到磁盘
    if (memtable.get_total_size() >= TomlConfig::getInstance().getLsmTolMemSizeLimit()) {
        return flush();
    }
    return 0;
}

uint64_t LSMEngine::remove_batch(const std::vector<std::string> &keys, uint64_t tranc_id) {
    // TODO: 批量删除
    // ? 调用 memtable.remove_batch(keys, tranc_id)
    // ? 若超限则 flush() 并返回其结果
    memtable.remove_batch(keys, tranc_id);

    spdlog::trace(
        "LSMEngine--"
        "remove_batch with {} keys tagged into memtable",
        keys.size());

    // 如果 memtable 太大，需要刷新到磁盘
    if (memtable.get_total_size() >= TomlConfig::getInstance().getLsmTolMemSizeLimit()) {
        return flush();
    }
    return 0;
}

void LSMEngine::clear() {
    memtable.clear();
    level_sst_ids.clear();
    ssts.clear();
    // 清空当前文件夹的所有内容
    try {
        for (const auto &entry : std::filesystem::directory_iterator(data_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::filesystem::remove(entry.path());

            spdlog::info(
                "LSMEngine--"
                "clear file {} successfully.",
                entry.path().string());
        }
    } catch (const std::filesystem::filesystem_error &e) {
        // 处理文件系统错误
        spdlog::error("Error clearing directory: {}", e.what());
    }

    // Re-create the vlog so new writes go to a fresh file
    if (vlog_) {
        vlog_->del_vlog();
        vlog_ = VLog::open(data_dir + "/vlog.data");
    }
}

uint64_t LSMEngine::flush() {
    // TODO: 刷盘保存sst文件
    // ? 0. 若 memtable 为空直接返回 0
    // ? 1. 加 ssts_mtx 写锁
    // ? 2. 若 L0 层 SST 数量 >= LsmSstLevelRatio, 先触发 full_compact(0)
    // ? 3. 分配新的 sst_id: next_sst_id++
    // ? 4. 构造 SSTBuilder:
    // ?    - 若 WiscKey 阈值 > 0 且 vlog_ 存在, 使用 WiscKey 模式的构造函数
    // ?    - 否则使用普通模式
    // ? 5. 调用 memtable.flush_last() 生成 SST 文件
    // ? 6. 更新 ssts 和 level_sst_ids[0] (push_front 保证新的在前)
    // ? 7. 将 flushed_tranc_ids 通知给 tran_manager
    // ? 8. 返回新 SST 的 max_tranc_id
    if (memtable.get_total_size() == 0) {
        return 0;
    }

    std::unique_lock<std::shared_mutex> lock(ssts_mtx);  // 写锁

    // 1. 先判断 level-0 sst 是否数量超限，超限就需要concat到 level-1
    if (level_sst_ids.find(0) != level_sst_ids.end() &&
        level_sst_ids[0].size() >= TomlConfig::getInstance().getLsmSstLevelRatio()) {
        full_compact(0);
    }

    // 2. 创建新的 SST ID
    size_t new_sst_id = next_sst_id++;

    // 3. 准备 SSTBuilder
    size_t wk = TomlConfig::getInstance().getWisckeyValueThreshold();
    SSTBuilder builder = (wk > 0 && vlog_) ? SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true, vlog_, wk)
                                           : SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true);

    // 4. 将 memtable 中最旧的表写入 SST
    auto sst_path = get_sst_path(new_sst_id, 0);
    auto new_sst = memtable.flush_last(builder, sst_path, new_sst_id, block_cache);

    // 5. 更新lsmengine中内存sst索引信息
    ssts[new_sst_id] = new_sst;

    // 6. 更新lsmengine中sst_ids
    level_sst_ids[0].push_front(new_sst_id);

    // 7. 添加到 flushed 集合（记录这次flush涉及到哪些事务）
    auto new_sst_max_tran_id = new_sst->get_tranc_id_range().second;
    auto tranmanager = tran_manager.lock();
    // 更新全局tranmanager和wal中的max_flushed_seq_
    tranmanager->update_max_flushed_seq(new_sst_max_tran_id);
    // 清除ready_to_flush数组中已经落盘的id
    // tranmanager->clean_ready_to_flush(new_sst_max_tran_id);

    // 返回新刷入的 sst 的最大的 tranc_id
    spdlog::info(
        "LSMEngine--"
        "Flush: Memtable flushed to SST with new sst_id={}, level=0",
        new_sst_id);
    return new_sst_max_tran_id;  // 返回本次落盘的sst中最大的tranc_id
}

// 获得落盘sst文件的路径
std::string LSMEngine::get_sst_path(size_t sst_id, size_t target_level) {
    // sst的文件路径格式为: data_dir/sst_<sst_id>，sst_id格式化为32位数字
    std::stringstream ss;
    ss << data_dir << "/sst_" << std::setfill('0') << std::setw(32) << sst_id << '.' << target_level;
    return ss.str();
}

std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>> LSMEngine::lsm_iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
    // TODO: 谓词查询
    // ? 1. 从 memtable 查询: memtable.iters_monotony_predicate(tranc_id,
    // predicate) ? 2. 遍历所有 SST, 对每个 SST 调用sst_iters_monotony_predicate
    // ?    将所有结果合并到 item_vec (注意过滤事务可见性和相同 key
    // 只保留最新版本) ? 3. 构造 TwoMergeIterator合并 memtable 结果和 sst 结果
    // ? 4. 若均为空返回 nullopt 先从 memtable 中查询
    // 1、从memtable中进行谓词查询
    auto mem_result = memtable.iters_monotony_predicate(tranc_id, predicate);

    // 2、从 sst 中查询
    std::vector<SearchItem> item_vec;
    for (auto &[sst_level, sst_ids] : level_sst_ids) {
        for (auto &sst_id : sst_ids) {
            auto sst = ssts[sst_id];
            auto result = SstIterator().sst_iters_monotony_predicate(sst, tranc_id, predicate);
            if (!result.has_value()) {
                continue;
            }
            spdlog::trace(
                "LSMEngine--"
                "lsm_iters_monotony_predicate(tranc_id={}): find a range "
                "from l{} sst{}",
                tranc_id, sst_level, sst_id);

            auto [it_begin, it_end] = result.value();
            for (; it_begin != it_end && it_begin.is_valid(); ++it_begin) {
                // l0中, 这里越古老的sst的idx越小, 我们需要让新的sst优先在堆顶
                // 让新的sst(拥有更大的idx)排序在前面, 反转符号就行了
                if (tranc_id != 0 && it_begin.get_tranc_id() > tranc_id) {
                    // 如果开启了事务, 比当前事务 id 更大的记录是不可见的
                    continue;
                }
                if (!item_vec.empty() && item_vec.back().key_ == it_begin.key()) {
                    // 如果key相同，则只保留最新的事务修改的记录即可
                    // 且这个记录既然已经存在于item_vec中，则其肯定满足了事务的可见性判断
                    continue;
                }
                item_vec.emplace_back(it_begin.key(), it_begin.value(), -sst_id, sst_level, it_begin.get_tranc_id());
            }
        }
    }
    std::shared_ptr<HeapIterator> ss_iter_ptr = std::make_shared<HeapIterator>(item_vec, tranc_id);

    if (!mem_result.has_value() && item_vec.empty()) {
        return std::nullopt;
    }

    // 3、构造 TwoMergeIterator合并 memtable 结果和 sst 结果
    std::shared_ptr<HeapIterator> mem_start_ptr;
    if (mem_result.has_value()) {
        auto [mem_start, _] = mem_result.value();
        mem_start_ptr = std::make_shared<HeapIterator>(mem_start);
    } else {
        mem_start_ptr = std::make_shared<HeapIterator>();  // 明确 empty
    }
    // auto mem_start_ptr =std::make_shared<HeapIterator>();
    // if (mem_result.has_value()) {
    //     auto [mem_start, _] = mem_result.value();
    //     *mem_start_ptr = mem_start;
    // }
    auto start = TwoMergeIterator(mem_start_ptr, ss_iter_ptr, tranc_id);
    auto end = TwoMergeIterator{};
    return std::make_optional<std::pair<TwoMergeIterator, TwoMergeIterator>>(start, end);
}

Level_Iterator LSMEngine::begin(uint64_t tranc_id) {
    // TODO: 返回指向的begin位置的层级指针Level_Iterator
    return Level_Iterator(shared_from_this(), tranc_id);
}

Level_Iterator LSMEngine::end() {
    // TODO: 返回指向的end位置的层级指针Level_Iterator
    // ? 返回空的 Level_Iterator{}
    return Level_Iterator{};
}

// full_compact():SSTable各层的压缩整合
//   ├─ 递归到要压缩整合的最深层
//   ├─ 进行压缩
//   |     ├─ if level-0层与level-1层的压缩
//   |     └─ else level-1层以下的压缩整合
//   ├─ 清除level、level+1的旧sst
//   ├─ 更新最大层数cur_max_level
//   └─ 记录新sst信息至level_sst_ids、ssts
// !实际压缩整合过程是从下往上做，这样可以使得原本的compact(level,level+1)仅在最深层有
// !其它层实际上是compact(level)，因为level+1在递归处理过程中被清空了，
// !从上往下处理，会导致前一次重写的内容，在本次合并中再次重写，有严重的写放大问题
void LSMEngine::full_compact(size_t src_level) {
    // TODO: 负责完成整个 full compact
    // ? 1. 递归判断下一级 level 是否需要
    // compact(level_sst_ids[src_level+1].size() >= ratio) ? 2. 根据 src_level
    // 是否为 0分别调用 full_l0_l1_compact 或 full_common_compact ? 3. 删除旧
    // SST文件并从 ssts/level_sst_ids 中移除记录 ? 4. 将新的 SST
    // 加入level_sst_ids[src_level+1] 并排序 ? 5. 更新 cur_max_level

    // 将 src_level 的 sst 全体压缩到 src_level + 1
    // 递归地判断下一级 level 是否需要 full compact
    if (level_sst_ids[src_level + 1].size() >= TomlConfig::getInstance().getLsmSstLevelRatio()) {
        full_compact(src_level + 1);
    }

    spdlog::debug(
        "LSMEngine--"
        "Compaction: Starting full compaction from level{} to level{}",
        src_level, src_level + 1);

    // 获取源level和目标level的 sst_id
    auto old_level_id_x = level_sst_ids[src_level];
    auto old_level_id_y = level_sst_ids[src_level + 1];
    std::vector<std::shared_ptr<SST>> new_ssts;
    std::vector<size_t> lx_ids(old_level_id_x.begin(), old_level_id_x.end());
    std::vector<size_t> ly_ids(old_level_id_y.begin(), old_level_id_y.end());
    if (src_level == 0) {
        // l0这一层不同sst中的key有重叠, 需要额外处理
        new_ssts = full_l0_l1_compact(lx_ids, ly_ids);
    } else {
        new_ssts = full_common_compact(lx_ids, ly_ids, src_level + 1);
    }
    // 完成 compact 后移除旧的sst记录
    for (auto &old_sst_id : old_level_id_x) {
        ssts[old_sst_id]->del_sst();
        ssts.erase(old_sst_id);
    }
    for (auto &old_sst_id : old_level_id_y) {
        ssts[old_sst_id]->del_sst();
        ssts.erase(old_sst_id);
    }
    level_sst_ids[src_level].clear();
    level_sst_ids[src_level + 1].clear();

    // 更新最大层数的记录
    cur_max_level = (std::max)(cur_max_level, src_level + 1);

    // 添加新的sst
    for (auto &new_sst : new_ssts) {
        level_sst_ids[src_level + 1].push_back(new_sst->get_sst_id());
        ssts[new_sst->get_sst_id()] = new_sst;
    }
    // 此处没必要reverse了
    std::sort(level_sst_ids[src_level + 1].begin(), level_sst_ids[src_level + 1].end());

    spdlog::debug(
        "LSMEngine--"
        "Compaction: Finished compaction. New SSTs added at level{}",
        src_level + 1);
}

// full_l0_l1_compact():level-0构建HeapIterator,level-1构建SSTIterator,
// 用这两个Iterator，构建TwoMergeIterattor，传入LSMEngine::gen_sst_from_iter()进行压缩合并
std::vector<std::shared_ptr<SST>> LSMEngine::full_l0_l1_compact(std::vector<size_t> &l0_ids,
                                                                std::vector<size_t> &l1_ids) {
    // TODO: 负责完成 level-0 和 level-1 的 full compact
    // ? L0 各 SST 的 key 有重叠, 需要先通过 SstIterator::merge_sst_iterator合并
    // ? 再用 TwoMergeIterator 与 L1 的 ConcactIterator 合并
    // ? 最后调用gen_sst_from_iter 生成新的 SST 文件 (目标大小 = PerMemSizeLimit
    // *SstLevelRatio) !
    // 这里各个接口的keep_all_versions应该为false，以便实现合并过程中的去重
    std::vector<SstIterator> l0_iters;          // level-0中各个sst迭代器组成的数组
    std::vector<std::shared_ptr<SST>> l1_ssts;  // level-1中各个sst指针组成的数组
    bool keep_all_versions_ = false;

    for (auto id : l0_ids) {
        auto sst_it = ssts[id]->begin(0, keep_all_versions_);
        l0_iters.push_back(sst_it);
    }
    for (auto id : l1_ids) {
        l1_ssts.push_back(ssts[id]);
    }
    // l0 的sst之间的key有重叠, 需要合并得到大根堆的HeapIterator
    auto [l0_begin, l0_end] = SstIterator::merge_sst_iterator(l0_iters, 0, keep_all_versions_);

    std::shared_ptr<HeapIterator> l0_begin_ptr = std::make_shared<HeapIterator>(l0_begin);
    // *l0_begin_ptr = l0_begin;

    std::shared_ptr<ConcactIterator> old_l1_begin_ptr =
        std::make_shared<ConcactIterator>(l1_ssts, 0, keep_all_versions_);

    // 通过level-0的HeapIterator和level-1的ConcatIterator（可能为空）进行合并得到一组新的sst
    TwoMergeIterator l0_l1_begin(l0_begin_ptr, old_l1_begin_ptr, 0, keep_all_versions_);
    return gen_sst_from_iter(
        l0_l1_begin,
        TomlConfig::getInstance().getLsmPerMemSizeLimit() * TomlConfig::getInstance().getLsmSstLevelRatio(), 1);
}

// full_common_compact():构建本层及下层的SSTIterator,
// 用这两个Iterator，构建TwoMergeIterattor，传入LSMEngine::gen_sst_from_iter()进行压缩合并
std::vector<std::shared_ptr<SST>> LSMEngine::full_common_compact(std::vector<size_t> &lx_ids,
                                                                 std::vector<size_t> &ly_ids, size_t level_y) {
    // TODO: 负责完成其他相邻 level 的 full compact
    // ? Lx 和 Ly 都是有序不重叠的 SST, 直接用 ConcactIterator 遍历
    // ? 通过 TwoMergeIterator 合并后调用 gen_sst_from_iter
    // ! 这里各个接口的keep_all_versions应该为false，以便实现合并过程中的去重
    // TODO 需要补全已完成事务的滤除
    std::vector<std::shared_ptr<SST>> lx_iters;
    std::vector<std::shared_ptr<SST>> ly_iters;
    bool keep_all_versions_ = false;

    for (auto id : lx_ids) {
        lx_iters.push_back(ssts[id]);
    }
    for (auto id : ly_ids) {
        ly_iters.push_back(ssts[id]);
    }

    std::shared_ptr<ConcactIterator> old_lx_begin_ptr =
        std::make_shared<ConcactIterator>(lx_iters, 0, keep_all_versions_);

    std::shared_ptr<ConcactIterator> old_ly_begin_ptr =
        std::make_shared<ConcactIterator>(ly_iters, 0, keep_all_versions_);

    TwoMergeIterator lx_ly_begin(old_lx_begin_ptr, old_ly_begin_ptr, 0, keep_all_versions_);
    // TODO: 如果目标 level 的下一级 level+1 不存在,
    // 则为底层的level,可以清理掉删除标记
    return gen_sst_from_iter(lx_ly_begin, LSMEngine::get_sst_size(level_y), level_y);
}

std::vector<std::shared_ptr<SST>> LSMEngine::gen_sst_from_iter(BaseIterator &iter, size_t target_sst_size,
                                                               size_t target_level) {
    // TODO: 实现从迭代器构造新的 SST
    // ? 循环从迭代器取 key-value 写入 SSTBuilder
    // ? 当 estimated_size >= target_sst_size 时 (注意不能在相同
    // key的不同版本之间切分) ? 调用 builder.build() 生成 SST 并重置 builder ?
    // 迭代结束后若 builder 非空则再次 build ? 注意: WiscKey 模式下需使用带 vlog
    // 参数的 SSTBuilder 构造函数

    // TODO: 这里需要补全的是对已经完成事务的删除
    std::vector<std::shared_ptr<SST>> new_ssts;
    size_t wk = TomlConfig::getInstance().getWisckeyValueThreshold();
    auto new_sst_builder = (wk > 0 && vlog_) ? SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true, vlog_, wk)
                                             : SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true);

    while (iter.is_valid() && !iter.is_end()) {
        std::string cur_key = (*iter).first;
        new_sst_builder.add(cur_key, (*iter).second, iter.get_tranc_id());
        ++iter;

        // keep_all_versions = false时，level1以下的层内无重复key
        // 再加上由于TwoMergeIterator的++能层间key去重，故next_is_same_key一定是false
        // keep_all_versions =
        // true时，sst各层就可能出现重复key，但本程序不使用这个模式！
        bool next_is_same_key = iter.is_valid() && !iter.is_end() && (*iter).first == cur_key;
        // 下一个是不相同的key，且目前放的sst已经数据量大小已经超过了预设大小target_sst_size，则新开一个sst继续放数据
        if (!next_is_same_key && new_sst_builder.estimated_size() >= target_sst_size) {
            size_t sst_id = next_sst_id++;  // TODO: 后续优化并发性
            std::string sst_path = get_sst_path(sst_id, target_level);
            auto new_sst = new_sst_builder.build(sst_id, sst_path, this->block_cache);
            new_ssts.push_back(new_sst);

            spdlog::debug(
                "LSMEngine--"
                "Compaction: Generated new SST file with sst_id={}"
                "at level{}",
                sst_id, target_level);

            new_sst_builder = (wk > 0 && vlog_)
                                  ? SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true, vlog_, wk)
                                  : SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(),
                                               true);  // 重置builder
        }
    }

    // 只要builder里面存在没有落盘的数据，就要把它放到sst里面去。
    if (new_sst_builder.real_size() > 0) {
        size_t sst_id = next_sst_id++;  // TODO: 后续优化并发性
        std::string sst_path = get_sst_path(sst_id, target_level);
        auto new_sst = new_sst_builder.build(sst_id, sst_path, this->block_cache);
        new_ssts.push_back(new_sst);

        spdlog::debug(
            "LSMEngine--"
            "Compaction: Generated new SST file with sst_id={} "
            "at level{}",
            sst_id, target_level);
    }

    return new_ssts;
}

size_t LSMEngine::get_sst_size(size_t level) {
    if (level == 0) {
        return TomlConfig::getInstance().getLsmPerMemSizeLimit();
    } else {
        return TomlConfig::getInstance().getLsmPerMemSizeLimit() *
               static_cast<size_t>(std::pow(TomlConfig::getInstance().getLsmSstLevelRatio(), level));
    }
}

void LSMEngine::set_tran_manager(std::shared_ptr<TranManager> tran_manager) { this->tran_manager = tran_manager; }

// *********************** LSM ***********************
LSM::LSM(std::string path){
    // TODO: 控制WAL重放与组件的初始化
    // ? 1. 绑定 tran_manager 与 engine: 互相 set
    // ? 2. 调用 tran_manager_->check_recover() 获取需要重放的事务记录
    // ? 3. 遍历返回的 map<tranc_id, records>:
    // ?    - 若该 tranc_id 已在 flushed_tranc_ids 中则跳过 (已刷盘无需重放)
    // ?    - 否则根据 record.getOperationType() 调用 engine->put() 或 engine->remove() 
    // ? 4. 调用 tran_manager_->init_new_wal() 开启新的WAL文件准备接收新写入

    // 1、初始化区存储引擎engine和事务管理器tran_manager
    engine = std::make_shared<LSMEngine>(path);
    tran_manager_=std::make_shared<TranManager>(path);

    // 2、绑定关键指针
    tran_manager_->set_engine(engine);
    engine->set_tran_manager(tran_manager_);

    // 3、根据wal恢复未flush至memtable中
    recover_from_wal();

    // 4、初始化并绑定wal对象（开启wal文件）
    tran_manager_->init_new_wal();
}

LSM::~LSM() {
    // 将memtable中的数据输入落盘
    flush_all();
    // tran_manager_->write_tranc_info_file();
}

std::optional<std::string> LSM::get(const std::string &key) {
    // auto tranc_id = tran_manager_->get_next_global_seq();
    auto tranc_id = tran_manager_->get_global_seq();
    auto res = engine->get(key, tranc_id);

    if (res.has_value()) {
        return res.value().first;
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::optional<std::string>>> LSM::get_batch(const std::vector<std::string> &keys) {
    // 1. 获取事务ID
    // auto tranc_id = tran_manager_->get_next_global_seq();
    auto tranc_id = tran_manager_->get_global_seq();

    // 2. 调用 engine 的批量查询接口
    auto batch_results = engine->get_batch(keys, tranc_id);

    // 3. 构造最终结果
    std::vector<std::pair<std::string, std::optional<std::string>>> results;
    for (const auto &[key, value] : batch_results) {
        if (value.has_value()) {
            results.emplace_back(key, value->first);  // 提取值部分
        } else {
            results.emplace_back(key, std::nullopt);  // 键不存在
        }
    }
    return results;
}

// 无事务所属的单步插入会被视做一个事务，分配一个tranc_id
void LSM::put(const std::string &key, const std::string &value) {
    auto tranc_id = tran_manager_->get_next_global_seq();
    engine->put(key, value, tranc_id);
}

// 无事务所属的批插入也会被视做一个事务，分配一个tranc_id
void LSM::put_batch(const std::vector<std::pair<std::string, std::string>> &kvs) {
    auto tranc_id = tran_manager_->get_next_global_seq();
    engine->put_batch(kvs, tranc_id);
}
void LSM::remove(const std::string &key) {
    auto tranc_id = tran_manager_->get_next_global_seq();
    engine->remove(key, tranc_id);
}

void LSM::remove_batch(const std::vector<std::string> &keys) {
    auto tranc_id = tran_manager_->get_next_global_seq();
    engine->remove_batch(keys, tranc_id);
}

void LSM::recover_from_wal() {
    // 进行WAL的恢复
    // 1）先获取在WAL中未落盘的操作（所有大于最大已落盘id的Record）
    auto check_recover_res = tran_manager_->check_recover();
    // 记录重放过程中见到的最大 committed_seq，恢复结束后抬升全局 id，
    // 避免后续提交复用已重放的版本号（崩溃时 tranc_info_file 可能落后于 WAL）

    uint64_t max_replayed_seq = 0;
    // 2）重新写入memtable中
    for (auto &[comitted_seq, records] : check_recover_res) {
        if (comitted_seq <= tran_manager_->get_max_flushed_seq()) {
            continue;  // 小于或等于的说明均已经flushed了
        }
        max_replayed_seq = (std::max)(max_replayed_seq, comitted_seq);
        int i = 0;
        for (auto &record : records) {
            // 根据未落盘事务中的操作记录，逐条重新执行，以实现恢复效果
            if (record.getOperationType() == OperationType::OP_PUT) {
                engine->put(record.getKey(), record.getValue(), comitted_seq);
            } else if (record.getOperationType() == OperationType::OP_DELETE) {
                engine->remove(record.getKey(), comitted_seq);
            }
        }
        spdlog::debug(
            "LSMEngine--"
            "Recover: Recovered transaction with comitted_seq={}",
            comitted_seq);
    }
    if (max_replayed_seq > 0) {
        tran_manager_->bump_global_seq((std::max)(max_replayed_seq, tran_manager_->get_max_flushed_seq()));
    }
}

void LSM::clear() { engine->clear(); }

void LSM::flush() { auto max_tranc_id = engine->flush(); }

void LSM::flush_all() {
    // 只要memtable中有数据就刷盘
    // std::cout<<"get_total_size is" <<engine->memtable.get_total_size();
    while (engine->memtable.get_total_size() > 0) {
        auto max_tranc_id = engine->flush();
        // tran_manager_->update_max_flushed_seq(max_tranc_id);
    }
}

LSM::LSMIterator LSM::begin(uint64_t tranc_id) { return engine->begin(tranc_id); }

LSM::LSMIterator LSM::end() { return engine->end(); }

std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>> LSM::lsm_iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
    return engine->lsm_iters_monotony_predicate(tranc_id, predicate);
}

// 开启一个事务
std::shared_ptr<TranContext> LSM::begin_tran(const IsolationLevel &isolation_level) {
    auto tranc_context = tran_manager_->new_tranc(isolation_level);

    spdlog::info(
        "LSM--"
        "lsm_iters_monotony_predicate: Starting query for tranc_id={}",
        tranc_context->tranc_id_);

    return tranc_context;
}

void LSM::set_log_level(const std::string &level) { reset_log_level(level); }
}  // namespace tiny_lsm
