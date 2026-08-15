#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "block/block.h"
#include "block/block_cache.h"
#include "block/blockmeta.h"
#include "utils/bloom_filter.h"
#include "utils/files.h"
#include "vlog/vlog.h"
#include "sst/sst_iterator.h"

/**
 * SST文件的结构, 参考自 https://skyzh.github.io/mini-lsm/week1-04-sst.html
 * --------------------------------------------------------------------------------
 * |         data Section          |  Meta Section |   Bloom Section  |  Extra    |
 * --------------------------------------------------------------------------------
 * | data block | ... | data block |    data meta   |       ...        |    ...    |
 * --------------------------------------------------------------------------------
 *
 * Meta Section 的总体结构: data meta  是一个BlockMeta数组加上一些描述信息
 * ---------------------------------------------------------------
 * | num_entries (32) | BlockMeta | ... | BlockMeta | Hash (32) |
 * ---------------------------------------------------------------
 * 其中, num_entries 表示 BlockMeta数组的长度, Hash 是 BlockMeta
 * 数组的哈希值(只包括数组部分, 不包括 num_entries ), 用于校验 data meta 的完整性

 * 其中,BlockMeta元素由一个DataBlock的元数据/属性信息的二进制编码组成，结构如下:
 * ---------------------------------------------------------------------------------------------------
 * | offset(32) | 1st_key_len(16) | 1st_key(1st_key_len) | last_key_len(16) |last_key(last_key_len) |
 * ---------------------------------------------------------------------------------------------------
 *
 *
 * Extra Section 的字段：控制字段，用于快速定位各个SSTable中的各个部分，以及获取一些SSTable的相关信息
 * Footer layout (old, 24 bytes):
 *   [meta_offset : uint32]  @ size-24
 *   [bloom_offset: uint32]  @ size-20
 *   [min_tranc_id: uint64]  @ size-16
 *   [max_tranc_id: uint64]  @ size-8
 *
 * Footer layout (WiscKey, 26 bytes):
 *   [meta_offset : uint32]  @ size-26
 *   [bloom_offset: uint32]  @ size-22
 *   [min_tranc_id: uint64]  @ size-18
 *   [max_tranc_id: uint64]  @ size-10
 *   [storage_mode: uint8 ]  @ size-2   (0=inline, 1=WiscKey)
 *   [magic       : uint8 ]  @ size-1   (0x4B constant)
 */


namespace tiny_lsm {
// std::enable_shared_from_this<> : 让一个对象在“自身内部”安全地生成指向自己的 shared_ptr
// class enable_shared_from_this {
//     std::weak_ptr<T> weak_this;
// };对象内部偷偷存了一个 weak_ptr 指向自己：
// 使用shared_from_this()实际上会发生：return weak_this.lock();
class SST : public std::enable_shared_from_this<SST> {
    friend class SSTBuilder;
    friend std::optional<std::pair<SstIterator, SstIterator>> 
        SstIterator::sst_iters_monotony_predicate(
        std::shared_ptr<SST> sst, 
        uint64_t tranc_id,
        std::function<int(const std::string&)> predicate);

private:
    FileObj file;
    std::vector<BlockMeta> block_meta_vec;
    uint32_t bloom_offset;
    uint32_t meta_block_offset;
    size_t sst_id;
    std::string first_key;
    std::string last_key;
    std::shared_ptr<BloomFilter> bloom_filter;
    std::shared_ptr<BlockCache> block_cache;    //Block的全局缓存池
    uint64_t min_tranc_id_ = UINT64_MAX;    //当前sst中最大的事务id
    uint64_t max_tranc_id_ = 0; //当前sst中最大的事务id

    // WiscKey fields
    uint8_t storage_mode_ = 0;  // 0=inline, 1=WiscKey
    std::shared_ptr<VLog> vlog_;

public:
    // 打开sst文件（实际上是惰性加载，只会加载必要的元数据，不加载data block）
    static std::shared_ptr<SST> open(size_t sst_id, FileObj file,
                                     std::shared_ptr<BlockCache> block_cache,
                                     std::shared_ptr<VLog> vlog = nullptr);
    
    
                                     
    // 根据key返回迭代器
    SstIterator get(const std::string& key, uint64_t tranc_id);
    
    // 根据索引读取block，可能直接在Block缓存池中命中
    std::shared_ptr<Block> read_block(int64_t block_idx);

    // 找到key所在的block的idx
    int64_t find_block_idx(const std::string& key);

    void del_sst(){ file.del_file(); }

    // 返回sst中block的数量
    size_t num_blocks() const { return block_meta_vec.size(); }

    // 返回sst的首key
    std::string get_first_key() const { return first_key; }

    // 返回sst的尾key
    std::string get_last_key() const { return last_key; }

    // 返回sst的大小
    size_t sst_size() const { return file.size(); }

    // 返回sst的id
    size_t get_sst_id() const { return sst_id; }

    
    /**
     * @brief 若使用了键值分离，则可能要进一步将查询的的'value'做解析，在VLog文件中找到真正的value
     * @param raw_value 查询到的value
     * @return std::string 解析后的真正的value
     */
    std::string resolve_value(const std::string& raw_value) const;

    // Returns true 如果 该 SST 使用 WiscKey 键值分离
    bool is_wisckey() const { return storage_mode_ == 1; }

    // std::optional<std::pair<SstIterator, SstIterator>> iters_monotony_predicate(
    //     std::function<bool(const std::string&)> predicate);

    SstIterator begin(uint64_t tranc_id, bool keep_all_versions = false);
    SstIterator end();

    std::pair<uint64_t, uint64_t> get_tranc_id_range() const{
        return std::make_pair(min_tranc_id_, max_tranc_id_);
    }
};

class SSTBuilder {
private:
    Block block;
    std::string first_key;
    std::string last_key;
    std::vector<BlockMeta> block_meta_vec;  // 等同于Meta Section中BlockMeta数组
    std::vector<uint8_t> data;  // 等同于Block Section中的data block的数组
    size_t block_size;
    std::shared_ptr<BloomFilter> bloom_filter;
    uint64_t min_tranc_id_ = UINT64_MAX;
    uint64_t max_tranc_id_ = 0;

    // WiscKey fields
    uint8_t storage_mode_ = 0;
    std::shared_ptr<VLog> vlog_;
    size_t wisckey_threshold_ = 0;

public:
    // 创建一个sst构建器, 指定目标block的大小 (inline mode)
    SSTBuilder(size_t block_size, bool has_bloom);
    // WiscKey constructor: values larger than wisckey_threshold go to vlog
    SSTBuilder(size_t block_size, bool has_bloom, std::shared_ptr<VLog> vlog, size_t wisckey_threshold);

    // 添加一个key-value对
    void add(const std::string& key, const std::string& value, uint64_t tranc_id);
    // 估计sst的大小
    size_t estimated_size() const;
    // 实际的大小（也就是 已经放进的大小和未放进去的data大小）
    size_t real_size() const;
    // 完成当前block的构建, 即将block写入data, 并创建新的block
    void finish_block();
    // 构建sst, 将sst写入文件并返回SST描述类
    std::shared_ptr<SST> build(size_t sst_id, const std::string& path, std::shared_ptr<BlockCache> block_cache);
};
}  // namespace tiny_lsm
