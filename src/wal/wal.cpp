
#include "wal/wal.h"

#include <algorithm>
#include <cstdint>
#include <vector>


namespace tiny_lsm {
// 从零开始的初始化流程
WAL::WAL(const std::string& log_dir, size_t buffer_size,
         uint64_t max_flushed_seq, uint64_t clean_interval,
         uint64_t file_size_limit)
    : buffer_size_(buffer_size),
      max_flushed_seq_(max_flushed_seq),
      clean_interval_(clean_interval),
      file_size_limit_(file_size_limit),
      stop_cleaner_(false) {
    // TODO: 实现WAL的初始化流程
    // ? 1. 设置 active_log_path_ = log_dir + "/wal.0"
    // ? 2. 用 FileObj::open(active_log_path_, true) 打开或创建 WAL 文件
    // ? 3. 启动清理线程: cleaner_thread_ = std::thread(&WAL::cleaner, this)
    std::unique_lock<std::mutex> lock(mutex_); 
    uint64_t max_tag = 0;
    if (!std::filesystem::exists(log_dir)) {
        std::filesystem::create_directories(log_dir);
    }
    auto dir_iter =  std::filesystem::directory_iterator(log_dir);
    lock.unlock();  //主动释放锁

    for (const auto &entry : dir_iter) {
        // 如果不是普通文件
        if (!entry.is_regular_file()) continue;
        // 如果不是已wal.为前缀的文件
        std::string filename = entry.path().filename().string();
        if (filename.size() < 4 || filename.substr(0, 4) != "wal.") continue;
        // 从索引4开始取子串
        auto tag = std::stoull(filename.substr(4));
        if (tag > max_tag) { 
            max_tag = tag; 
        }
    }
    // 打开最大序号的wal，没有则新建wal.0
    active_log_path_ = log_dir + "/wal." + std::to_string(max_tag);

    // // !这里使用截断的方式似乎不太合理
    log_file_ = FileObj::open(active_log_path_, true);
    // if(std::filesystem::exists(active_log_path_)){
    //     log_file_ = FileObj::open(active_log_path_,false);
    // }else{
    //     log_file_ = FileObj::open(active_log_path_,true);
    // }
    if (log_file_.size() > file_size_limit_) {
        reset_file();  // 创建 wal.(max_tag+1)
    }
    // 开启一个后台清理线程，执行本对象中的cleaner函数
    cleaner_thread_ = std::thread(&WAL::cleaner, this);
}

WAL::~WAL() {
    // TODO: 实现WAL的清理流程
    // ? 1. 强制将缓冲区所有内容刷盘: log({}, true)
    // ? 2. 加锁设置 stop_cleaner_ = true
    // ? 3. 等待清理线程结束: cleaner_thread_.join()
    // ? 4. 显式关闭文件: log_file_.close()
    // 先将缓冲区所有内容强制刷入
    flush();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_cleaner_ = true;
    }

    // 确保线程被正确唤醒并退出
    if (cleaner_thread_.joinable()) {
        cleaner_thread_.join();
    }
    // 显式关闭文件
    log_file_.close(); // 显式关闭文件
}

std::map<uint64_t, std::vector<Record>> WAL::recover(
    const std::string& log_dir, uint64_t max_flushed_seq) {
    // TODO: 检查需要重放的WAL日志
    // ? 1. 若 log_dir 不存在则直接返回空 map
    // ? 2. 遍历目录找到所有 "wal." 前缀的文件
    // ? 3. 按 seq 升序排序
    // ? 4. 逐文件读取所有 Record (Record::decode)
    // ?    仅保留 tranc_id > max_flushed_seq 的记录
    // ? 5. 返回 map<tranc_id, records>
    std::map<uint64_t, std::vector<Record>> tranc_records{};
    // 引擎启动时判断
    if (!std::filesystem::exists(log_dir)) {
        return tranc_records;
    }

    // 遍历log_dir下的所有文件
    std::vector<std::string> wal_paths;
    for (const auto &entry : std::filesystem::directory_iterator(log_dir)) {
        if (entry.is_regular_file()) {
            // 获取/符号后的文件名
            std::string filename = entry.path().filename().string();
            if (filename.substr(0, 4) != "wal.") {
                continue;
            }
            wal_paths.push_back(entry.path().string());
        }
    }
    // 按照升序排序
    std::sort(wal_paths.begin(), wal_paths.end(),
                [](const std::string &a, const std::string &b) {
                    auto a_seq_str = a.substr(a.find_last_of(".") + 1);
                    auto b_seq_str = b.substr(b.find_last_of(".") + 1);
                    return std::stoi(a_seq_str) < std::stoi(b_seq_str);
                });

    // 读取所有的记录
    for (const auto &wal_path : wal_paths) {
        auto wal_file = FileObj::open(wal_path, false);
        auto wal_records_slice = wal_file.read_to_slice(0, wal_file.size());
        auto records = Record::decode(wal_records_slice);
        for (const auto &record : records) {
            // Record的tranc_id 大于 max_flushed_seq, 才需要尝试恢复
            if (record.getTrancId() > max_flushed_seq) {
                tranc_records[record.getTrancId()].push_back(record);
            }
        }
    }
    return tranc_records;
}

// WAL日志刷盘
void WAL::flush() {
    // TODO: 强制刷盘
    // ? 当前实现仅需加锁保证当前写入完成即可
    // ? 若 log() 中使用了缓冲区, 这里需要确保缓冲区内容全部落盘
    log({}, true);
}

void WAL::reset_max_flushed_seq(uint64_t seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_flushed_seq_ = seq;
}

void WAL::log(const std::vector<Record>& records, bool force_flush) {
    // TODO: 实现WAL的写入流程
    // ? 1. 加锁
    // ? 2. 将 records 追加到 log_buffer_
    // ? 3. 若 log_buffer_.size() < buffer_size_ 且 !force_flush 则直接返回
    // ? 4. 否则将 log_buffer_ 中所有记录编码并写入 log_file_ (record.encode())
    // ? 5. 调用 log_file_.sync() 确保落盘
    // ? 6. 若文件大小超过 file_size_limit_ 则调用 reset_file() 滚动日志文件
    std::unique_lock<std::mutex> lock(mutex_);
        // 将 records 的所有记录添加到 log_buffer_
    for (const auto &record : records) {
        log_buffer_.push_back(record);
    }
    // 这是为了满足分批次刷盘的需要
    if (log_buffer_.size() < buffer_size_ && !force_flush) {
        // 如果 log_buffer_ 的大小小于 buffer_size_ 且 force_flush 为 false,
        // 不进行写入
        return;
    }
    // 刷入 wal 日志文件中
    auto pre_buffer = std::move(log_buffer_);
    for (const auto &record : pre_buffer) {
        std::vector<uint8_t> encoded_record = record.encode();
        log_file_.append(encoded_record);
    }
    if (!log_file_.sync()) {
        // 确保WAL日志立即写入磁盘
        throw std::runtime_error("Failed to sync WAL file");
    }
    auto cur_file_size = log_file_.size();
    // WAL日志文件大小超过预设，则新建一个新的WAL日志
    if (cur_file_size > file_size_limit_) {
        reset_file();
    }
}

void WAL::cleaner() {
    // TODO:  实现WAL的清理线程
    // ? 循环:
    // ?   1. sleep clean_interval_ 秒
    // ?   2. 若 stop_cleaner_ 为 true 则退出
    // ?   3. 调用 cleanWALFile() 清理已可以删除的旧 WAL 文件
    while (true) {
    {
        // 定时清理旧的WAL文件
        std::this_thread::sleep_for(std::chrono::seconds(clean_interval_));
        if (stop_cleaner_) {
            break;
        }
        cleanWALFile();
    }
    }
}

void WAL::cleanWALFile() {
    // 遍历log_file_所在的文件夹
    std::unique_lock<std::mutex> lock(mutex_);  // 只在获取当前文件路径时获取锁
    // 取wal.文件的文件夹路径
    std::filesystem::path p = active_log_path_;
    std::string dir_path = p.parent_path().string();
    if(dir_path.empty())
        dir_path = "./";
    else
        dir_path += "/";
    lock.unlock();  //主动释放锁

    // wal文件格式为:wal.{seq}
    std::vector<std::pair<size_t, std::string>> wal_paths;  //sqe-->wal的路径
    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        // 如果不是普通文件
        if (!entry.is_regular_file()) 
            continue;
        // 如果不是已wal.为前缀的文件
        std::string filename = entry.path().filename().string();
        if (filename.size() < 4 || filename.substr(0, 4) != "wal.") 
            continue;
        size_t dot_pos = filename.find_last_of(".");    //就是3
        uint64_t seq = std::stoull(filename.substr(dot_pos + 1));
        wal_paths.push_back({seq, entry.path().string()});
    }

    // 按照seq升序排序
    std::sort(wal_paths.begin(), wal_paths.end(),
              [](const std::pair<size_t, std::string>& a,
                 const std::pair<size_t, std::string>& b) {
                  return a.first < b.first;
              });

    // 判断是否可以删除
    std::vector<FileObj> del_paths;
    for (int idx = 0; idx < wal_paths.size() - 1; idx++) {
        auto cur_path = wal_paths[idx].second;
        auto cur_file = FileObj::open(cur_path, false);
        // 遍历文件记录, 读取所有的tranc_id,
        // 判断是否都小于等于max_flushed_seq_
        size_t offset = 0;
        bool has_unfinished = false;
        while (offset + sizeof(uint16_t) < cur_file.size()) {
            uint16_t record_size = cur_file.read_uint16(offset);
            uint64_t tranc_id = cur_file.read_uint64(offset + sizeof(uint16_t));
            if (tranc_id > max_flushed_seq_) {
                has_unfinished = true;
                break;
            }
            offset += record_size;
        }
        if (!has_unfinished) {
            del_paths.push_back(std::move(cur_file));
        }
    }
    // 在上述扫描结束后，进行删除过时的Wal
    for (auto& del_file : del_paths) {
        del_file.del_file();
    }
}

// 当前wal文件容量超出阈值后, 创建新的文件, 将seq自增
void WAL::reset_file() {
    // wal文件格式为: wal.{seq}
    auto old_path = active_log_path_;
    // 字符串处理获取seq
    auto seq = std::stoi(old_path.substr(old_path.find_last_of(".") + 1));
    seq++;
    active_log_path_ = old_path.substr(0, old_path.find_last_of(".")) + "." +
                       std::to_string(seq);
    // 创建新的文件
    log_file_ = FileObj::create_and_write(active_log_path_, {});
}
}  // namespace tiny_lsm
