#pragma once

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tiny_lsm {

enum class OperationType {
    OP_CREATE,
    OP_COMMIT,
    OP_ROLLBACK,
    OP_PUT,
    OP_DELETE,
};

// Record 就是 WAL（Write-Ahead Log）里的“一条日志记录”的抽象
// 把一次数据库操作（事务/写入/删除）变成“可持久化、可恢复”的二进制日志。
class Record {
// 构造器私有化，禁止其直接构造，只能通过static函数构造
private:
    Record() = default;

public:
    static Record createRecord(uint64_t tranc_id);
    static Record commitRecord(uint64_t tranc_id);
    static Record rollbackRecord(uint64_t tranc_id);
    static Record putRecord(uint64_t tranc_id, const std::string& key,
                            const std::string& value);
    static Record deleteRecord(uint64_t tranc_id, const std::string& key);
    
    // encode函数是以单个Record为单位, 将其编码为字节流
    // 而decode函数是以字节流为单位, 将其解码为Record数组。
    std::vector<uint8_t> encode() const;
    static std::vector<Record> decode(const std::vector<uint8_t>& data);

    uint64_t getTrancId() const { return tranc_id_; }
    void setTrancid(uint64_t commit_seq) {tranc_id_ =  commit_seq;}
    OperationType getOperationType() const { return operation_type_; }
    std::string getKey() const { return key_; }
    std::string getValue() const { return value_; }

    void print() const;

    bool operator==(const Record& other) const;
    bool operator!=(const Record& other) const;

private:
    uint64_t tranc_id_;
    OperationType operation_type_;
    std::string key_;
    std::string value_;
    uint16_t record_len_;
};
}  // namespace tiny_lsm