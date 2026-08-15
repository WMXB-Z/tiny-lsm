#include "block/blockmeta.h"

#include <cstring>
#include <functional>
#include <stdexcept>

namespace tiny_lsm {
BlockMeta::BlockMeta() : offset(0), first_key(""), last_key("") {}

BlockMeta::BlockMeta(size_t offset, const std::string& first_key, const std::string& last_key)
    : offset(offset), first_key(first_key), last_key(last_key) {}

void BlockMeta::encode_meta_to_slice(std::vector<BlockMeta>& block_meta_vec, std::vector<uint8_t>& metadata) {
    // TODO:  将内存中所有`Blcok`的元数据编码为二进制字节数组
    // ? 输入输出都由参数中的引用给定, 你不需要自己创建`vector`

    // 1. Mete段总大小：num_entries(32) + 所有entries的大小 + hash(32)
    uint32_t num_entries = block_meta_vec.size();
    size_t total_size = sizeof(uint32_t);  // num_entries

    // 计算所有entries的大小
    for (const auto& meta : block_meta_vec) {
        total_size += sizeof(uint32_t) +       // offset
                      sizeof(uint16_t) +       // first_key_len
                      meta.first_key.size() +  // first_key
                      sizeof(uint16_t) +       // last_key_len
                      meta.last_key.size();    // last_key
    }
    total_size += sizeof(uint32_t);  // hash

    // 2. 分配空间
    metadata.resize(total_size);
    uint8_t* ptr = metadata.data();

    // 3. 写入元素个数
    memcpy(ptr, &num_entries, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    // 4. 写入每个entry
    for (const auto& meta : block_meta_vec) {
        // 写入 offset
        uint32_t offset32 = static_cast<uint32_t>(meta.offset);
        memcpy(ptr, &offset32, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        // 写入 first_key_len 和 first_key
        uint16_t first_key_len = meta.first_key.size();
        memcpy(ptr, &first_key_len, sizeof(uint16_t));
        ptr += sizeof(uint16_t);
        memcpy(ptr, meta.first_key.data(), first_key_len);
        ptr += first_key_len;

        // 写入 last_key_len 和 last_key
        uint16_t last_key_len = meta.last_key.size();
        memcpy(ptr, &last_key_len, sizeof(uint16_t));
        ptr += sizeof(uint16_t);
        memcpy(ptr, meta.last_key.data(), last_key_len);
        ptr += last_key_len;
    }

    // 5. 计算并写入hash
    const uint8_t* data_start = metadata.data() + sizeof(uint32_t);
    const uint8_t* data_end = ptr;
    size_t data_len = data_end - data_start;
    // 使用 std::hash 计算哈希值
    uint32_t hash = std::hash<std::string_view>{}( std::string_view(reinterpret_cast<const char*>(data_start), data_len));

    memcpy(ptr, &hash, sizeof(uint32_t));
}

std::vector<BlockMeta> BlockMeta::decode_meta_from_slice(const std::vector<uint8_t>& datameta) {
    // TODO:将二进制字节数组解码为内存中的`Blcok Meta`数据
    std::vector<BlockMeta> block_meta_vec;

    // 1. 验证最小长度
    if (datameta.size() < sizeof(uint32_t) * 2) {  // 至少要有num_entries和hash
        throw std::runtime_error("Invalid metadata size");
    }

    // 2. 读取元素个数
    uint32_t num_entries;
    const uint8_t* ptr = datameta.data();
    memcpy(&num_entries, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    // 3. 读取entries
    for (uint32_t i = 0; i < num_entries; ++i) {
        BlockMeta meta;

        // 读取 offset
        uint32_t offset32;
        memcpy(&offset32, ptr, sizeof(uint32_t));
        meta.offset = offset32;
        ptr += sizeof(uint32_t);

        // 读取 first_key
        uint16_t first_key_len;
        memcpy(&first_key_len, ptr, sizeof(uint16_t));  //memcpy按字节赋值
        ptr += sizeof(uint16_t);
        //容器的复制接口：清空原内容，必要时会重新分配内存
        meta.first_key.assign(reinterpret_cast<const char*>(ptr), first_key_len);   
        ptr += first_key_len;

        // 读取 last_key
        uint16_t last_key_len;
        memcpy(&last_key_len, ptr, sizeof(uint16_t));
        ptr += sizeof(uint16_t);
        meta.last_key.assign(reinterpret_cast<const char*>(ptr), last_key_len);
        ptr += last_key_len;

        block_meta_vec.push_back(meta);
    }

    // 4. 验证hash
    uint32_t stored_hash;
    memcpy(&stored_hash, ptr, sizeof(uint32_t));

    const uint8_t* data_start = datameta.data() + sizeof(uint32_t);
    const uint8_t* data_end = ptr;
    size_t data_len = data_end - data_start;

    // 使用与编码时相同的 std::hash 计算哈希值
    uint32_t computed_hash = std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(data_start), data_len));
    if (stored_hash != computed_hash) {
        throw std::runtime_error("Metadata hash mismatch");
    }

    return block_meta_vec;
}
}  // namespace tiny_lsm