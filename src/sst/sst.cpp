#include "sst/sst.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>

#include "config/config.h"
#include "consts.h"
#include "sst/sst_iterator.h"

namespace tiny_lsm {

// Magic byte identifying a WiscKey SST footer
static constexpr uint8_t WISCKEY_MAGIC = 0x4B;
// Old footer size (24 bytes)
static constexpr size_t OLD_FOOTER_SIZE = sizeof(uint32_t) * 2 + sizeof(uint64_t) * 2;
// New WiscKey footer size (26 bytes)
static constexpr size_t WISCKEY_FOOTER_SIZE = OLD_FOOTER_SIZE + 2;

// **************************************************
// SST
// **************************************************
// SST对数据的查询是惰性地从文件系统中进行读取, 但必要的元信息会预先加载到内存中。
// SST::open():从磁盘加载一个 SST 文件中的meta信息，构建一个可查询的内存对象SST（但不加载全部数据）
//   ├─ 根据已打开的 FileObj file，先确定SST的编码格式
//   ├─ 读取并记录最大、最小事务id至SST中
//   ├─ 读取BloomSection的偏移量、Meta Section的偏移量，并记录至SST中
//   ├─ 读取 bloom filter，并记录SST中
//   ├─ 读取并解码Meta Section，并记录至SST中
//   ├─ 第一个key和最后一个key，记录至sst中
//   └─ 返回这个内存实例SST的指针
std::shared_ptr<SST> SST::open(size_t sst_id, FileObj file, std::shared_ptr<BlockCache> block_cache,
                               std::shared_ptr<VLog> vlog) {
    // TODO: 打开一个SST文件, 返回一个描述类
    // [data blocks][meta block][bloom filter][footer]对该SST文件，只读取meta block+bloom filter+footer
    // ? 步骤:
    // ?   0. 检测文件末尾 magic byte 判断是否为 WiscKey 格式 (WISCKEY_MAGIC = 0x4B)
    // ?        footer 共 24 字节 (老格式) 或 26 字节 (WiscKey, 末尾多storage_mode + magic)
    // ?   1. 从文件末尾读取 footer: meta_block_offset,bloom_offset, min_tranc_id, max_tranc_id
    // ?      如为 WiscKey 格式,还需读取 storage_mode_
    // ?   2. 读取并解码 Bloom Filter (bloom_offset ~ meta_block_offset 之间)
    // ?   3. 读取并解码元数据块 (meta_block_offset ~ bloom_offset 之间)
    // ?      调用 BlockMeta::decode_meta_from_slice
    // ?   4.设置 first_key 和 last_key
    // ?   注: vlog 用于 WiscKey 模式下的 value 读取,直接赋值给 sst->vlog_
    auto sst = std::make_shared<SST>();
    sst->sst_id = sst_id;
    sst->file = std::move(file);
    sst->block_cache = block_cache;
    sst->vlog_ = vlog;

    size_t file_size = sst->file.size();
    // 读取文件末尾的footer段，若连旧格式24都没有，说明SST有问题
    if (file_size < OLD_FOOTER_SIZE) {
        throw std::runtime_error("Invalid SST file: too small");
    }

    // 判断是否为 WiscKey 格式：最后一个字节（魔术字节）为0x4B && file大小超过26B
    size_t footer_size = OLD_FOOTER_SIZE;
    if (file_size >= WISCKEY_FOOTER_SIZE && sst->file.read_uint8(file_size - 1) == WISCKEY_MAGIC) {
        // 从文件尾部（按 WiscKey footer 格式）尝试读取 meta_offset
        uint32_t candidate_meta_offset = 0;
        auto candidate_bytes = sst->file.read_to_slice(file_size - WISCKEY_FOOTER_SIZE, sizeof(uint32_t));
        memcpy(&candidate_meta_offset, candidate_bytes.data(), sizeof(uint32_t));

        // 进一步判断，meta section必须在footer之前，这是为了防止普通 SST 文件最后一个字节“可能刚好也是 0x4B
        // 如果满足则它就是WISCKEY式，否则认为就是旧格式
        if (candidate_meta_offset < file_size - WISCKEY_FOOTER_SIZE) {
            footer_size = WISCKEY_FOOTER_SIZE;
            sst->storage_mode_ = sst->file.read_uint8(file_size - 2);
        }
    }

    // 0. 读取最大和最小的事务id
    auto max_tranc_id =
        sst->file.read_to_slice(file_size - footer_size + OLD_FOOTER_SIZE - sizeof(uint64_t), sizeof(uint64_t));
    memcpy(&sst->max_tranc_id_, max_tranc_id.data(), sizeof(uint64_t));

    auto min_tranc_id =
        sst->file.read_to_slice(file_size - footer_size + OLD_FOOTER_SIZE - sizeof(uint64_t) * 2, sizeof(uint64_t));
    memcpy(&sst->min_tranc_id_, min_tranc_id.data(), sizeof(uint64_t));

    // 1. 读取BloomSection的偏移量、Meta Section的偏移量记录至SST中
    auto bloom_offset_bytes = sst->file.read_to_slice(
        file_size - footer_size + OLD_FOOTER_SIZE - sizeof(uint64_t) * 2 - sizeof(uint32_t), sizeof(uint32_t));
    memcpy(&sst->bloom_offset, bloom_offset_bytes.data(), sizeof(uint32_t));

    auto meta_offset_bytes = sst->file.read_to_slice(
        file_size - footer_size + OLD_FOOTER_SIZE - sizeof(uint64_t) * 2 - sizeof(uint32_t) * 2, sizeof(uint32_t));
    memcpy(&sst->meta_block_offset, meta_offset_bytes.data(), sizeof(uint32_t));

    // 2. 读取 bloom filter，并记录SST中
    if (sst->bloom_offset + OLD_FOOTER_SIZE < file_size) {
        uint32_t bloom_size = file_size - footer_size - sst->bloom_offset;
        if (bloom_size > 0) {
            auto bloom_bytes = sst->file.read_to_slice(sst->bloom_offset, bloom_size);
            auto bloom = BloomFilter::decode(bloom_bytes);
            sst->bloom_filter = std::make_shared<BloomFilter>(std::move(bloom));
        }
    }

    // 3. 读取并解码Meta Section，并记录至SST中
    uint32_t meta_size = sst->bloom_offset - sst->meta_block_offset;
    auto meta_bytes = sst->file.read_to_slice(sst->meta_block_offset, meta_size);
    sst->block_meta_vec = BlockMeta::decode_meta_from_slice(meta_bytes);

    // 4. 根据Meta Section中的信息，获得data Seciton中的第一个key和最后一个key，记录至sst中
    if (!sst->block_meta_vec.empty()) {
        sst->first_key = sst->block_meta_vec.front().first_key;
        sst->last_key = sst->block_meta_vec.back().last_key;
    }

    return sst;
}

std::shared_ptr<Block> SST::read_block(int64_t block_idx) {
    // TODO: 根据 block 的 id 读取一个 Block
    // ? 先从 block_cache 查找; 未命中则计算该 block 的偏移和大小
    // ? 读取数据后调用 Block::decode(data, true) 解码
    // ? 解码后存入 block_cache 并返回
    // ? block 大小: 相邻 block_meta_vec 的 offset 差值; 最后一个 block 到 meta_block_offset
    if (block_idx >= static_cast<int64_t>(block_meta_vec.size())) {
        throw std::out_of_range("Block index out of range");
    }

    // 先从缓存中查找
    if (block_cache != nullptr) {
        auto cache_ptr = block_cache->get(this->sst_id, block_idx);
        if (cache_ptr != nullptr) {
            return cache_ptr;
        }
    } else {
        throw std::runtime_error("Block cache not set");
    }

    const auto& meta = block_meta_vec[block_idx];
    size_t block_size;

    // 计算block大小
    if (block_idx == static_cast<int64_t>(block_meta_vec.size()) - 1) {
        block_size = meta_block_offset - meta.offset;
    } else {
        block_size = block_meta_vec[block_idx + 1].offset - meta.offset;
    }

    // 读取block数据并解码
    auto block_data = file.read_to_slice(meta.offset, block_size);
    auto block_res = Block::decode(block_data, true);

    // 更新缓存
    if (block_cache != nullptr) {
        block_cache->put(this->sst_id, block_idx, block_res);
    } else {
        throw std::runtime_error("Block cache not set");
    }
    return block_res;
}

int64_t SST::find_block_idx(const std::string& key) {
    // TODO:  二分查找，找到key所在的Block的索引（已通过）
    // ? 先用布隆过滤器快速排除 (bloom_filter->possibly_contains(key))
    // ? 再在 block_meta_vec 上二分查找: first_key <= key <= last_key
    // ? 若未找到合适 block 返回 -1

    // 在布隆过滤器判断key是否存在
    if (bloom_filter != nullptr && !bloom_filter->possibly_contains(key)) {
        return -1;
    }

    // 二分查找
    int64_t left = 0;
    int64_t right = block_meta_vec.size();

    while (left < right) {
        int64_t mid = (left + right) / 2;
        const auto& meta = block_meta_vec[mid];

        if (key < meta.first_key) {
            right = mid;
        } else if (key > meta.last_key) {
            left = mid + 1;
        } else {
            return mid;
        }
    }

    if (left >= static_cast<int64_t>(block_meta_vec.size())) {
        // 如果没有找到完全匹配的块，返回-1
        return -1;
    }
    return left;
}

SstIterator SST::get(const std::string& key, uint64_t tranc_id) {
    // TODO: 根据查询 key 返回一个SstIterator迭代器
    // ? 先检查 key 是否在 [first_key, last_key] 范围内, 超出范围则返回 end()
    // ? 再用 bloom_filter 快速排除 key的存在性
    // ? 返回 SstIterator(shared_from_this(), key, tranc_id)
    if (key < first_key || key > last_key) {
        return this->end();
    }

    // 在布隆过滤器判断key是否存在(这一步在SST::find_block_idx中执行)
    if (bloom_filter != nullptr && !bloom_filter->possibly_contains(key)) {
        return this->end();
    }

    return SstIterator(shared_from_this(), key, tranc_id);
}

// 解析出Entry中的真实value
std::string SST::resolve_value(const std::string& raw_value) const {
    // WiscKey 模式下:
    // raw_value 是 12 字节的 vlog 引用 [offset:8][size:4]
    // 普通模式下直接返回 raw_value
    if (storage_mode_ == 0 || raw_value.empty()) {
        return raw_value;
    }
    // 判断是否是小value，是的话，就直接返回即可
    // 因为这里设计的是：指针大小(8B)+偏移量(4B)，如果连12字节都没有，则说明它是小字节
    if (raw_value.size() < 12) {
        return raw_value;
    }
    uint64_t off = 0;
    uint32_t sz = 0;
    memcpy(&off, raw_value.data(), sizeof(uint64_t));
    memcpy(&sz, raw_value.data() + sizeof(uint64_t), sizeof(uint32_t));
    if (!vlog_) {
        throw std::runtime_error("SST::resolve_value: vlog is null for WiscKey SST");
    }
    // 大字节value会通过vlog_实现访问
    return vlog_->read_value(off, sz);
}

// keep_all_versions=false 时只保留每个 key 的最新版本（事务可见版本）
// keep_all_versions=true 时用于 compact，保留全部历史版本
SstIterator SST::begin(uint64_t tranc_id, bool keep_all_versions) {
    // TODO: 返回起始位置迭代器
    return SstIterator(shared_from_this(), tranc_id, keep_all_versions);
}

SstIterator SST::end() {
    // TODO: 返回终止位置迭代器
    // ? 构造一个 SstIterator 并将 m_block_idx 设为 block_meta_vec.size(), m_block_it 设为 nullptr
    SstIterator res(shared_from_this(), 0);
    res.m_block_idx = block_meta_vec.size();  // 表示无效Block索引
    res.m_block_it = nullptr;
    return res;
}

// **************************************************
// SSTBuilder
// **************************************************

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom) : block(block_size) {
    // 初始化第一个block
    if (has_bloom) {
        bloom_filter = std::make_shared<BloomFilter>(TomlConfig::getInstance().getBloomFilterExpectedSize(),
                                                     TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
    }
    block_meta_vec.clear();
    data.clear();
    first_key.clear();
    last_key.clear();
}

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom, std::shared_ptr<VLog> vlog, size_t wisckey_threshold)
    : block(block_size), vlog_(std::move(vlog)), wisckey_threshold_(wisckey_threshold), storage_mode_(1) {
    // WiscKey 模式构造函数: vlog 用于大 value 分离存储
    if (has_bloom) {
        bloom_filter = std::make_shared<BloomFilter>(TomlConfig::getInstance().getBloomFilterExpectedSize(),
                                                     TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
    }
    block_meta_vec.clear();
    data.clear();
    first_key.clear();
    last_key.clear();
}

// add()：不断往当前 block 塞 KV，并 判断是否需要切块
//   ├─ 尝试往当前 block中写数据
//   ├─ 如果 block 无法写入（例如写满了、同key无法放入一个block等） → finish_block()
//   │       ├─ block 编码进 data数组
//   │       ├─ 记录该block的meta信息 至 block_meta_vec数组
//   │       └─ 开启新 block
//   ├─ 写入数据
//   └─ 维护SSTBuilder的一些控制信息（first_key / last_key、bloom filter、tranc_id 范围）
void SSTBuilder::add(const std::string& key, const std::string& value, uint64_t tranc_id) {
    // TODO: 添加键值对
    // ? 记录 first_key (第一次调用时)
    // ? 向 bloom_filter 中 add key
    // ? 更新 max_tranc_id_ / min_tranc_id_
    // ? WiscKey 模式下: 若 value 非空且超过 wisckey_threshold_, 将 value 写入vlog
    // ? 并将 vlog 引用 [offset:8][size:4] 作为 actual_value
    // ? 尝试向 block 添加 entry; 若返回 false (block满) 先调用 finish_block() 再添加
    // ? 注意: 相同 key 必须在同一个 block 中 (force_write = key == last_key)
    // ? 更新 last_key

    // 记录第一个key
    if (first_key.empty()) {
        first_key = key;
    }

    // 在 布隆过滤器 中添加key
    if (bloom_filter != nullptr) {
        bloom_filter->add(key);
    }

    // 记录 事务id 范围
    max_tranc_id_ = (std::max)(max_tranc_id_, tranc_id);
    min_tranc_id_ = (std::min)(min_tranc_id_, tranc_id);

    // WiscKey: 一种优化机制，SST中只存小value，将“大value”从会分离出去
    // 即如果value太大，就不直接存进SST，而是存到vlog，然后在SST里只保存一个“指针（offset + size）”。
    const std::string* actual_value = &value;
    std::string vlog_ref;

    // !判断是否启用 WiscKey + 是否是大 value，如果是大字节value，
    // 则会将实际的value追加至vlog_文件中，sst文件中仅保留它在vlog中的偏移地址(8B)和字节大小(4B)
    if (storage_mode_ == 1 && vlog_ && !value.empty() && wisckey_threshold_ > 0 && value.size() >= wisckey_threshold_) {
        uint64_t offset = vlog_->append(key, value);
        vlog_ref.resize(sizeof(uint64_t) + sizeof(uint32_t));
        uint32_t val_size = static_cast<uint32_t>(value.size());
        memcpy(vlog_ref.data(), &offset, sizeof(uint64_t));
        memcpy(vlog_ref.data() + sizeof(uint64_t), &val_size, sizeof(uint32_t));
        actual_value = &vlog_ref;
    }

    bool force_write = (key == last_key);
    // 连续出现相同的 key 必须位于 同一个 block 中

    if (block.add_entry(key, *actual_value, tranc_id, force_write)) {
        // block 满足容量限制, 插入成功
        last_key = key;
        return;
    }

    finish_block();  // 将当前 block 写入 data数组中

    block.add_entry(key, *actual_value, tranc_id, false);
    first_key = key;
    last_key = key;  // 更新最后一个key
}

size_t SSTBuilder::real_size() const { return data.size() + block.cur_size(); }

size_t SSTBuilder::estimated_size() const { return data.size(); }

void SSTBuilder::finish_block() {
    // TODO: 构建Block
    // ? 将当前 block 编码并追加到 data, 同时向 block_meta_vec 添加元数据
    // ? 然后重置 block 为新的空 Block
    // ? block_meta_vec 记录: (当前data起始偏移, first_key, last_key)

    auto old_block = std::move(this->block);  // 触发移动语义，this->block中部分内容被置空
    auto encoded_block = old_block.encode();

    block_meta_vec.emplace_back(data.size(), first_key, last_key);
    // 预分配空间并添加数据
    // data.reserve(data.size() + encoded_block.size());
    data.insert(data.end(), encoded_block.begin(), encoded_block.end());
}

// build()：把所有 block + meta + footer 拼成一个完整 SST 文件
//   ├─ 如果还有没写完的 block → finish_block()
//   ├─ 写 data blocks 至 file（要先将最后一个block写入data中）
//   ├─ 写 meta block 至 file
//   │       └─ 需要将block_meta_vec数组 编码成 meta block
//   ├─ 写 extra 至 file（该过程含有一些extra信息需要的处理工作）
//   │       ├─ 编码布隆过滤器
//   │       ├─ 添加元数据块偏移量
//   │       ├─ 添加布隆过滤器偏移量
//   │       └─ 添加其他控制信息
//   ├─ file落盘生成实际的FileObj file
//   └─ 根据SSTBuilder中的信息构建一个SST实例
std::shared_ptr<SST> SSTBuilder::build(size_t sst_id, const std::string& path,
                                       std::shared_ptr<BlockCache> block_cache) {
    // TODO: 构建一个SST
    // ? 1. 若 block 非空则调用 finish_block()
    // ? 2. 若 block_meta_vec 为空则抛出异常
    // ? 3. 编码元数据块，将data + meta block写入file中 (BlockMeta::encode_meta_to_slice)
    // ? 4. 追加 Bloom Filter 编码
    // ? 5. 写入 footer (老格式 24B 或 WiscKey 26B):
    // ?    [meta_offset:uint32][bloom_offset:uint32][min_tranc_id:uint64][max_tranc_id:uint64]
    // ?    WiscKey 额外: [storage_mode_:uint8][WISCKEY_MAGIC:uint8]
    // ? 6. 调用 FileObj::create_and_write 写文件
    // ? 7. 构造并返回 SST 对象（记录上述过程的一些关键信息作为SST中的控制信息）

    // 收尾工作：将最后一个装有数据的block放入编码的二进制data数组中
    if (!block.is_empty()) {
        finish_block();
    }

    // 如果没有数据，抛出异常
    if (block_meta_vec.empty()) {
        throw std::runtime_error("Cannot build empty SST");
    }

    // 编码元数据块(block_meta_vec数组-->meta section对应的metadata)
    std::vector<uint8_t> meta_block;  // 即metadata
    BlockMeta::encode_meta_to_slice(block_meta_vec, meta_block);

    // 计算元数据块的偏移量
    uint32_t meta_offset = data.size();

    // 构建完整的文件内容
    // 1. 添加数据块（data section)
    std::vector<uint8_t> file_content = std::move(data);

    // 2. 添加元数据块（Meta Section）
    file_content.insert(file_content.end(), meta_block.begin(), meta_block.end());

    // 3. 添加其余块（Extra）
    // 1) 编码布隆过滤器：用来快速判断一个 key “一定不存在” 或 “可能存在”，减少IO次数
    uint32_t bloom_offset = file_content.size();
    if (bloom_filter != nullptr) {
        auto bf_data = bloom_filter->encode();
        file_content.insert(file_content.end(), bf_data.begin(), bf_data.end());
    }

    // Footer: 24 bytes (老格式) or 26 bytes (WiscKey格式)
    // 这些字节位于文件末，表示“文件的目录入口/索引入口”，打开 SST 时，不会从头扫描整个文件（太慢）
    // 而是直接从文件末尾读取 Footer，快速定位关键数据位置（如meta section）
    bool is_wisckey = (storage_mode_ == 1);
    size_t extra_len = OLD_FOOTER_SIZE + (is_wisckey ? 2 : 0);
    file_content.resize(file_content.size() + extra_len);

    uint8_t* footer_base = file_content.data() + file_content.size() - extra_len;

    // 2) 添加元数据块偏移量
    memcpy(footer_base, &meta_offset, sizeof(uint32_t));

    // 3) 添加布隆过滤器偏移量
    memcpy(footer_base + sizeof(uint32_t), &bloom_offset, sizeof(uint32_t));

    // 4) 添加最大和最小的事务id
    memcpy(footer_base + sizeof(uint32_t) * 2, &min_tranc_id_, sizeof(uint64_t));
    memcpy(footer_base + sizeof(uint32_t) * 2 + sizeof(uint64_t), &max_tranc_id_, sizeof(uint64_t));

    // 5) WiscKey格式有额外的两个字节
    if (is_wisckey) {
        file_content[file_content.size() - 2] = storage_mode_;
        file_content[file_content.size() - 1] = WISCKEY_MAGIC;
    }

    // 创建文件
    FileObj file = FileObj::create_and_write(path, file_content);

    // 返回SST对象
    auto res = std::make_shared<SST>();

    res->sst_id = sst_id;
    res->file = std::move(file);
    res->first_key = block_meta_vec.front().first_key;
    res->last_key = block_meta_vec.back().last_key;
    res->meta_block_offset = meta_offset;
    res->bloom_filter = this->bloom_filter;
    res->bloom_offset = bloom_offset;
    res->block_meta_vec = std::move(block_meta_vec);
    res->block_cache = block_cache;
    res->max_tranc_id_ = max_tranc_id_;
    res->min_tranc_id_ = min_tranc_id_;
    res->storage_mode_ = storage_mode_;
    res->vlog_ = vlog_;

    return res;
}
}  // namespace tiny_lsm
