#include "vlog/vlog.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "spdlog/spdlog.h"

namespace tiny_lsm {

// Simple CRC32 实现 (polynomial多项式 0xEDB88320)
static uint32_t crc32_compute(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFF;
}

std::shared_ptr<VLog> VLog::open(const std::string& path) {
    // TODO: 打开或创建 VLog 文件
    // ? 1. 若文件不存在则创建空文件
    // ? 2. 用 FileObj::open(path, false) 打开（不截断，保留已有记录）
    // ? 3. 记录 path_ 和 file_
    auto vlog = std::make_shared<VLog>();
    vlog->path_ = path;

    if (!std::filesystem::exists(path)) {
        // 用 ofstream 以二进制模式创建一个空文件
        std::ofstream f(path, std::ios::binary);
    }
    vlog->file_ = FileObj::open(path, false);
    
    // if (!std::filesystem::exists(path)){
    //     vlog->file_ = FileObj::open(path, true);
    // } else{
    //     vlog->file_ = FileObj::open(path, false);
    // }
    spdlog::info("VLog::open: opened vlog at {}, size={}", path,
                 vlog->file_.size());
    return vlog;
}

uint64_t VLog::append(const std::string& key, const std::string& value) {
    // TODO: 追加一条 K-V 记录到 VLog，返回该k-v记录起始偏移量
    // ? 加 append_mtx_ 互斥锁（支持并发写）
    // ? offset = file_.size()（追加前的文件大小即为本次记录的起始偏移）
    // ? 记录格式: [key_len:uint16][key][val_len:uint32][value][crc32:uint32]
    // ? CRC32 覆盖除自身之外的所有字段
    // ? 使用 file_.append(buf) 写入
    std::lock_guard<std::mutex> lock(append_mtx_);
    uint64_t offset = file_.size();
    // Build the record buffer
    // [key_len:2][key:key_len][val_len:4][value:val_len][crc32:4]
    uint16_t key_len = static_cast<uint16_t>(key.size());
    uint32_t val_len = static_cast<uint32_t>(value.size());
    size_t record_size = sizeof(uint16_t) + key_len + sizeof(uint32_t) +
                         val_len + sizeof(uint32_t);

    std::vector<uint8_t> buf(record_size);
    size_t pos = 0;

    memcpy(buf.data() + pos, &key_len, sizeof(uint16_t));
    pos += sizeof(uint16_t);

    memcpy(buf.data() + pos, key.data(), key_len);
    pos += key_len;

    memcpy(buf.data() + pos, &val_len, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    memcpy(buf.data() + pos, value.data(), val_len);
    pos += val_len;

    // CRC校验码
    uint32_t crc = crc32_compute(buf.data(), pos);
    memcpy(buf.data() + pos, &crc, sizeof(uint32_t));

    if (!file_.append(buf)) {
        throw std::runtime_error("VLog::append: failed to write record");
    }

    spdlog::trace(
        "VLog::append: wrote record at offset={}, key_len={}, "
        "val_len={}",
        offset, key_len, val_len);

    return offset;
}

std::string VLog::read_value(uint64_t offset, uint32_t value_size) {
    // TODO: 根据指定的offset定位至对应的k-v处，并读取 value
    // ? 先读 key_len (uint16_t, 2 bytes) 以跳过 key
    // ?即： value 起始位置 = offset + 2 + key_len + 4，读取 value_size 个字节返回
    // Record layout: [key_len:2B][key:key_len][val_len:4B][value:val_len][crc:4B]
    // 先读取key_len以跳过key，然后读取val_len，从而获得真正的value部分
    auto key_len_bytes = file_.read_to_slice(offset, sizeof(uint16_t));
    uint16_t key_len = 0;
    memcpy(&key_len, key_len_bytes.data(), sizeof(uint16_t));

    // value部分的起始位置: offset + 2 + key_len + 4
    uint64_t val_offset =
        offset + sizeof(uint16_t) + key_len + sizeof(uint32_t);

    // 其实只需要通过value的offset就能定位到value，但原sst中也保存了val_len这4B
    auto val_bytes = file_.read_to_slice(val_offset, value_size);
    return std::string(val_bytes.begin(), val_bytes.end());
}

uint64_t VLog::tail_offset() const {
    return static_cast<uint64_t>(file_.size());
}

void VLog::sync() { file_.sync(); }

void VLog::del_vlog() { file_.del_file(); }

}  // namespace tiny_lsm
