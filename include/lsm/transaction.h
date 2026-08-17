#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>

#include "utils/files.h"
#include "wal/wal.h"

namespace tiny_lsm {

// 事务隔离级别
enum class IsolationLevel {
    READ_UNOP_COMMITTED,
    READ_OP_COMMITTED,
    REPEATABLE_READ,
    SERIALIZABLE
};

// 事务状态
enum class TransactionState { OP_COMMITTED, ABORTED };

inline std::string isolation_level_to_string(const IsolationLevel& level);

class LSMEngine;
class TranManager;

// 事务上下文（或称为事务句柄）：主要用于保存事务执行过程中的上下文信息，执行事务过程
class TranContext {
    friend class TranManager;

public:
    TranContext(uint64_t tranc_id, std::shared_ptr<LSMEngine> engine,
                std::shared_ptr<TranManager> tranManager,
                const enum IsolationLevel& isolation_level);
    void put(const std::string& key, const std::string& value); //事务内部的插入
    void remove(const std::string& key);    //事务内部的删除
    std::optional<std::string> get(const std::string& key);

    // test_fail = true 用于测试中手动触发的崩溃
    bool commit(bool test_fail = false);    //事务提交
    bool abort();       
    enum IsolationLevel get_isolation_level();

public:
    std::shared_ptr<LSMEngine> engine_; // LSMEngine引擎的指针
    std::weak_ptr<TranManager> tranManager_; // 事务管理器的指针
    uint64_t tranc_id_; //事务id
    std::vector<Record> operations; // 事务操作Record数组, 也就是后续转化为WAL日志的内容
    // temp_map_:事务执行过程中还未提交的数据, 例如事务中的put操作的k-v数据暂存到这里，等到后续写入memtable
    std::unordered_map<std::string, std::string> temp_map_; 
    bool isCommited = false;
    bool isAborted = false;
    enum IsolationLevel isolation_level_;

private:
    std::unordered_map<std::string, std::optional<std::pair<std::string, uint64_t>>> read_map_;  // 保存从engine中读入的k-v
};

// TranManager: 分配事务 ID + 写WAL + 崩溃恢复
class TranManager : public std::enable_shared_from_this<TranManager> {
public:
    TranManager(std::string data_dir);
    ~TranManager();
    void init_new_wal();
    void set_engine(std::shared_ptr<LSMEngine> engine);
    std::shared_ptr<TranContext> new_tranc(const IsolationLevel& isolation_level);

    uint64_t get_global_seq();
    uint64_t get_next_global_seq();
    uint64_t get_max_flushed_seq();

    void update_max_flushed_seq(uint64_t tranc_id);
    // void add_ready_to_flush(uint64_t tranc_id, TransactionState state);
    // void clean_ready_to_flush(uint64_t tranc_id);

    bool write_to_wal(const std::vector<Record>& records);

    std::map<uint64_t, std::vector<Record>> check_recover();
    std::string get_tranc_info_file_path();
    void write_tranc_info_file();
    void read_tranc_info_file();

private:
    mutable std::mutex mutex_;
    std::shared_ptr<LSMEngine> engine_; // 与全局的LSMEngine绑定
    std::shared_ptr<WAL> wal;       // 与全局的Wal绑定
    std::string data_dir_;  // 记录事务信息文件所在目录
    std::atomic<uint64_t> global_seq_ = 0;   // 全局事务id计数器
    std::atomic<uint64_t> max_flushed_seq_ = 0;   // 全局已flush序号
    // std::map<uint64_t, std::shared_ptr<TranContext>> activeTrans_; // 记录事务id-->事务句柄的映射表
    // std::map<uint64_t, TransactionState> ready_to_flush_;  //记录执行commit或abort后待被flush至sst中的事务，id-->事务句柄
    FileObj tranc_info_file_;         // 全局的事务信息文件
};

}  // namespace tiny_lsm