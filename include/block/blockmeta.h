#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * SST文件的结构, 参考自 https://skyzh.github.io/mini-lsm/week1-04-sst.html
 * 整个SST对应的实体类有三部分：Block数据段（Block section） + 索引段/元数据段（Meta Section）+ 补充段（Extra）
 *  Block数据段采用延迟加载方式——只有在用户请求时才会加载到内存中，而索引段会被提前加载到内存作为控制结构
 * -------------------------------------------------------------------------------------------
 * |         Block Section         |          Meta Section         | Extra                  |
 * -------------------------------------------------------------------------------------------
 * | data block | ... | data block |            block meta          | meta block offset (32) |
 * -------------------------------------------------------------------------------------------

 * 其中, block meta 是一个BlockMeta数组加上一些描述信息, BlockMeta 结构如下:
 * --------------------------------------------------------------------------------------------------------------
 * | offset (32) | first_key_len (16) | first_key (first_key_len) | last_key_len(16) | last_key (last_key_len) |
 * --------------------------------------------------------------------------------------------------------------

 * Meta Section 的结构如下:
 * -------------------------------------------------------------------
 * | num_entries (32) | BlockMeta | ... | BlockMeta | Hash (32) |
 * ---------------------------------------------------------------
 * 其中, num_entries 表示 metadata 数组的长度, Hash 是 metadata
数组的哈希值(只包括数组部分, 不包括 num_entries ), 用于校验 metadata 的完整性
 */

namespace tiny_lsm {
class BlockMeta {
    friend class BlockMetaTest;
public:
    BlockMeta();
    BlockMeta(size_t offset, const std::string& first_key,const std::string& last_key);
    static void encode_meta_to_slice(std::vector<BlockMeta>& block_meta_vec, std::vector<uint8_t>& metadata);
    static std::vector<BlockMeta> decode_meta_from_slice(const std::vector<uint8_t>& metadata);

public:
    size_t offset;          // 块在文件中的偏移量
    std::string first_key;  // 块的第一个key
    std::string last_key;   // 块的最后一个key
};
}  // namespace tiny_lsm
