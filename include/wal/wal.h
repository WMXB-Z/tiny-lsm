#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "record.h"
#include "utils/files.h"

namespace tiny_lsm {

class WAL {
public:
    WAL(const std::string& log_dir, size_t buffer_size,
        uint64_t max_flushed_seq, uint64_t clean_interval,
        uint64_t file_size_limit);
    ~WAL();
    
    // 恢复WAL文件, 返回WAL记录
    static std::map<uint64_t, std::vector<Record>> recover(
        const std::string& log_dir, uint64_t max_flushed_seq);

    // 将记录添加到缓冲区或者刷入磁盘的WAL文件中 (取决于你的策略选择性使用)
    void log(const std::vector<Record>& records, bool force_flush = false);

    // 强制将缓冲区中的记录刷入磁盘的WAL文件中 (取决于你的策略选择性使用)
    void flush();

    void reset_max_flushed_seq(uint64_t max_flushed_seq);

private:
    void cleaner();  //清理旧数据的线程（选择性使用）
    void cleanWALFile();
    void reset_file();

protected:
    std::string active_log_path_;   //当前写入的WAL文件路径
    FileObj log_file_;  // 当前写入的WAL文件对象 
    size_t file_size_limit_;    // WAL文件的大小限制（选择性使用）
    std::mutex mutex_;  
    std::vector<Record> log_buffer_;    //记录的缓冲区（选择性使用）
    size_t buffer_size_;    //缓冲区的大小（选择性使用）
    std::thread cleaner_thread_;    //清理线程（选择性使用）
    uint64_t max_flushed_seq_ = 0;  // 已刷盘至SST中的最大事务id，低于此id的WAL记录可以被celan
    std::atomic<bool> stop_cleaner_;
    uint64_t clean_interval_;   //后台 cleaner 线程“多久执行一次 WAL 清理”的时间间隔
};
}  // namespace tiny_lsm