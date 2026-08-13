#include "lsm/transaction.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "lsm/engine.h"
#include "spdlog/spdlog.h"
#include "utils/files.h"

namespace tiny_lsm {

inline std::string isolation_level_to_string(const IsolationLevel& level) {
    switch (level) {
        case IsolationLevel::READ_UNOP_COMMITTED:
            return "READ_UNOP_COMMITTED";
        case IsolationLevel::READ_OP_COMMITTED:
            return "READ_OP_COMMITTED";
        case IsolationLevel::REPEATABLE_READ:
            return "REPEATABLE_READ";
        case IsolationLevel::SERIALIZABLE:
            return "SERIALIZABLE";
        default:
            return "UNKNOWN";
    }
}

// *********************** TranContext 事务句柄 ***********************
TranContext::TranContext(uint64_t tranc_id, std::shared_ptr<LSMEngine> engine,
                         std::shared_ptr<TranManager> tranManager,
                         const enum IsolationLevel& isolation_level)
    : tranc_id_(tranc_id),
      engine_(std::move(engine)),
      tranManager_(tranManager),
      isolation_level_(isolation_level) {
    // TODO: Lab 5.2 事务句柄初始化
    // "创建操作"的记录对象，放入操作记录数组中
    operations.emplace_back(Record::createRecord(tranc_id_));
}

// put(): 完成事务中的put操作
//   ├─ 创建Record对象，并记录
//   └─ 暂存k-v至temp_map_中，等待事务的commit/abort
void TranContext::put(const std::string& key, const std::string& value) {
    // TODO: Lab 5.2 put 实现
    spdlog::trace(
        "LSM--"
        "lsm_iters_monotony_predicate: Starting query for tranc_id={}",
        this->tranc_id_);

    auto isolation_level = get_isolation_level();

    // 所有隔离级别都需要先写入 operations 中
    operations.emplace_back(Record::putRecord(this->tranc_id_, key, value));

    // 2 其他隔离级别需要 暂存到 temp_map_ 中, 统一提交后才在数据库中生效
    temp_map_[key] = value;

    spdlog::trace("TranContext--{}: put({}, {}) stored in temp map",
                  isolation_level_to_string(isolation_level_), key, value);
}

// 实现上与put完全相同
void TranContext::remove(const std::string& key) {
    // TODO: Lab 5.2 remove 实现
    spdlog::trace("TranContext--remove({}) called, tranc_id={}", key,
                  tranc_id_);

    auto isolation_level = get_isolation_level();

    // 所有隔离级别都需要先写入 operations 中
    operations.emplace_back(Record::deleteRecord(this->tranc_id_, key));

    // 2 其他隔离级别需要 暂存到 temp_map_ 中, 统一提交后才在数据库中生效
    temp_map_[key] = "";
    spdlog::trace("TranContext--{}: remove({}) stored in temp map",
                  isolation_level_to_string(isolation_level_), key);
}

std::optional<std::string> TranContext::get(const std::string& key) {
    // TODO: Lab 5.2 get 实现
    spdlog::trace("TranContext--get({}) called, tranc_id={}", key, tranc_id_);
    auto isolation_level = get_isolation_level();

    // 1 所有隔离级别先就近在当前操作的临时缓存中查找
    // 读已提交不保留读取的结果，故会跳过
    if (temp_map_.find(key) != temp_map_.end()) {
        spdlog::trace("TranContext--{}: get({}) found in temp map",
                      isolation_level_to_string(isolation_level), key);
        return temp_map_[key];
    }

    // 2 为查询成功，说明此处是第一次查询，使用 engine 查询
    std::optional<std::pair<std::string, uint64_t>> query;
    if (isolation_level == IsolationLevel::READ_OP_COMMITTED) {
        // 2.2 如果隔离级别是 READ_OP_COMMITTED, 使用 engine 查询时判断 tranc_id
        query = engine_->get(key, 0);
    } else if(isolation_level == IsolationLevel::REPEATABLE_READ ||
            isolation_level == IsolationLevel::SERIALIZABLE) {
        // 2.2 如果隔离级别是 SERIALIZABLE 或 REPEATABLE_READ, 第一次使用 engine
        // 查询后还需要将值暂存至上下文中
        // !存在问题，仅靠当前事务的id是不足以判断key是否已提交的，无法判断key对当前事务的可见性
        // !要真正实现key对当前事务的可见性判断，应该使用“事务可见性集合（committed set/snapshot）”
        // !也不能直接使用“已提交的事务id集合”，事务id的大小并不能决定提交的时间次序（版本新旧）
        query = engine_->get(key, this->tranc_id_);
        // 将从engine_中读入的key保存在对应缓存
        read_map_[key] = query;
    }

    if (query.has_value()) {
        spdlog::trace("TranContext--{}: get({}) returned value={}",
                      isolation_level_to_string(isolation_level), key,
                      query->first);
    } else {
        spdlog::trace("TranContext--{}: get({}) returned no value",
                      isolation_level_to_string(isolation_level), key);
    }
    return query.has_value() ? std::make_optional(query->first) : std::nullopt;
}


// commit：事务提交时要冲突检测, 如果无冲突且WAL持久化成功, 返回true，否则返回false
//   ├─ Read-Write冲突检测(仅对RP和SE做，因为它们要求可重复读，不能基于过期数据做决策)
//   ├─ 将本事务的Record数组写入WAL文件
//   ├─ 将本事务的k-v写入memtable
//   └─ 修改tranManager控制信息
bool TranContext::commit(bool test_fail) {
    // TODO: Lab 5.2 commit 实现
    // 事务提交 = 逻辑上已经成功 + 满足持久性保证（写入了WAL）
    spdlog::info("TranContext--commit(): Starting commit for transaction ID={}",
                 tranc_id_);

    auto isolation_level = get_isolation_level();
    MemTable& memtable = engine_->memtable;
    auto tranManager = tranManager_.lock();
    // 1、Write-Write冲突检测（仅 RR / SERIALIZABLE）
    // 检查“事务读过的数据，在事务执行期间有没有被别人改”，如果有返回false
    // 检查：是否存在“另一个已提交事务”在snapshot 之后修改了这个 key
    if (isolation_level == IsolationLevel::REPEATABLE_READ ||
        isolation_level == IsolationLevel::SERIALIZABLE) {
        std::unique_lock<std::shared_mutex> wlock1(memtable.cur_mtx);
        std::unique_lock<std::shared_mutex> wlock2(memtable.frozen_mtx);
        // TODO: 目前为检查冲突, 全局获取了读锁, 后续考虑性能优化方案
        std::shared_lock<std::shared_mutex> rlock3(engine_->ssts_mtx);
        for (auto& [k, v] : temp_map_) {
            // memtable 冲突
            auto res = memtable.get_(k, 0); //无锁get_
            // !这个判断可见性的方式存在问题
            if (res.is_valid() && res.get_tranc_id() > tranc_id_) {
                spdlog::warn("TranContext--commit(): Conflict detected on key={}, aborting ID={}", k, tranc_id_);
                isAborted = true;
                // tranManager->add_ready_to_flush(tranc_id_, TransactionState::ABORTED);
                return false;
            }
            // sst 冲突(避免非事务单步操作先于事务做了修改)
            auto sst_res = engine_->sst_get_(k, 0); //无锁get_
            // !这个判断可见性的方式存在问题
            if (sst_res.has_value() && sst_res->second > tranc_id_) {
                spdlog::warn("TranContext--commit(): SST conflict on key={}, aborting ID={}", k, tranc_id_);
                isAborted = true;
                // tranManager->add_ready_to_flush(tranc_id_, TransactionState::ABORTED);
                return false;
            }
        }
    }

    // 2、Read-Write冲突检测（仅SERIALIZABLE）
    // ! 这里尚未完全实现可串行化的隔离级别
    if (isolation_level == IsolationLevel::SERIALIZABLE) {
        for (auto& [k, v] : read_map_) {
            if(!v.has_value())
                continue;
            auto read_tran_id = v.value().second;
            // memtable中检查冲突
            auto res = memtable.get_(k, 0); //无锁get_
            // !这个判断可见性的方式存在问题
            if (res.is_valid() && res.get_tranc_id() > read_tran_id) {
                spdlog::warn("RW Conflict: Conflict detected on key={}, ID={}, but read ID={}", 
                    k, res.get_tranc_id(), read_tran_id);
                isAborted = true;
                // tranManager->add_ready_to_flush(tranc_id_, TransactionState::ABORTED);
                return false;
            }
            // sst中冲突
            auto sst_res = engine_->sst_get_(k, 0); //无锁get_
            // !这个判断可见性的方式存在问题
            if (sst_res.has_value() && sst_res->second > read_tran_id) {
                spdlog::warn("RW Conflict: Conflict detected on key={}, ID={}, but read ID={}", 
                    k,res.get_tranc_id(), read_tran_id);
                isAborted = true;
                // tranManager->add_ready_to_flush(tranc_id_, TransactionState::ABORTED);
                return false;
            }
        }
    }
    // 2、对Record数组中的tranc_id进行改写，改写为commit_seq，写入 WAL
    auto committed_seq = tranManager->get_next_global_seq();
    for(auto& record : operations){
        record.setTrancid(committed_seq);
    }
    operations.emplace_back(Record::commitRecord(committed_seq));
    if (!tranManager->write_to_wal(operations)) {
        spdlog::error("TranContext--commit(): WAL write failed, tran_ID={}, commitedseq={}", tranc_id_, committed_seq);
        throw std::runtime_error("write to wal failed");
    }

    // 3、写memtable，test_fail是用于测试中，故意让其写入memtable失败 
    {
        std::unique_lock<std::shared_mutex> wlock1(memtable.cur_mtx);
        std::unique_lock<std::shared_mutex> wlock2(memtable.frozen_mtx);
        // 3、应用写入，从事务私有缓存WriteBatch（这里是temp_map_）中，将k-v逐个写入memtable
        if (!test_fail) {
            // 这里是手动调用 memtable 的无锁版本的 put_, 因为之前手动加了写锁
            for (auto& [k, v] : temp_map_) {
                // memtable 必须与 WAL 使用相同的 committed_seq，
                // 否则刷盘后的 max_flushed_seq 无法覆盖 WAL 中已提交的事务，
                // 导致重启恢复时把已落盘数据全部重放一遍。
                memtable.put_(k, v, committed_seq);
            }
        }
    }

    // 4、标记提交完成
    isCommited = true;
    // 在事务待刷入数组中，添加一条OP_COMMITTED事务
    // tranManager->add_ready_to_flush(committed_seq, TransactionState::OP_COMMITTED);
    spdlog::info("TranContext--commit(): Committed successfully, tran_ID={}, commitedseq={}",  tranc_id_, committed_seq);
    return true;
}

bool TranContext::abort() {
    // TODO: Lab 5.2 abort（事务的回滚）实现 
    spdlog::info("TranContext--abort(): Aborting transaction ID={}", tranc_id_);

    auto isolation_level = get_isolation_level();
    auto tranManager = tranManager_.lock();
    // abort 收尾逻辑
    isAborted = true;
    // 在事务待刷入数组中，添加一条ABORTED事务(什么也不做)
    // tranManager->add_ready_to_flush(tranc_id_, TransactionState::ABORTED);
    spdlog::info("TranContext--abort(): Transaction ID={} aborted", tranc_id_);
    return true;
}

// 返回事务隔离级别
enum IsolationLevel TranContext::get_isolation_level() {
    return isolation_level_;
}

// *********************** TranManager ***********************
TranManager::TranManager(std::string data_dir) : data_dir_(data_dir) {
    // TODO: Lab 5.2 初始化时，从持久化的文件中恢复事务的状态信息
    auto file_path = get_tranc_info_file_path();
    // 判断文件是否存在
    if (!std::filesystem::exists(file_path)) {
        // 以创建的方式打开文件
        tranc_info_file_ = FileObj::open(file_path, true);

    } else {
        // 打开存在的文件
        tranc_info_file_ = FileObj::open(file_path, false);
        read_tranc_info_file();
    }
}

void TranManager::init_new_wal() {
    spdlog::info("TranManager--init_new_wal(): Cleaning up old WAL files");
    // TODO: 1 和 4096 应该统一用宏定义
    // !注意：这里有问题，如果事务恢复后还未持久化，但这里又被删除，那将会发生数据丢失!
    // !不应该主动删除，可以留给clearn线程清理
    // for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
    //     if (entry.path().filename().string().find("wal.") == 0) {
    //         std::filesystem::remove(entry.path());
    //     }
    // }
    wal = std::make_shared<WAL>(data_dir_, 128, get_max_flushed_seq(), 1, 4096);
    // flushedTrancIds_.clear();   // 清空"已落盘事务id"集合
    // flushedTrancIds_.insert(global_seq_.load() - 1); 
    spdlog::info("TranManager--init_new_wal(): New WAL initialized");
}

void TranManager::set_engine(std::shared_ptr<LSMEngine> engine) {
    engine_ = std::move(engine);
}

TranManager::~TranManager() { write_tranc_info_file(); }

// 事务管理器关闭时，将本次事务执行后的状态信息写入事务信息文件中包括：
// 下一个新建事务将分配的id号、已刷盘事务数量和id
void TranManager::write_tranc_info_file() {
    // TODO: Lab 5.2 持久化事务状态信息
    int buffer_size = sizeof(uint64_t) * 2;
    std::vector<uint8_t> buf(buffer_size, 0);
    uint64_t global_seq = global_seq_.load();
    uint64_t max_flushed_seq = max_flushed_seq_.load();
    memcpy(buf.data(), &global_seq, sizeof(uint64_t));
    memcpy(buf.data() + sizeof(uint64_t), &max_flushed_seq, sizeof(uint64_t));
    // 注意这里的写方式是从0开始写，这样保证了状态文件tranc_info_file_的写入方式是“覆盖写”
    tranc_info_file_.write(0, buf);
    tranc_info_file_.sync();
}

// 从事务信息文件中读取配置信息
void TranManager::read_tranc_info_file() {
    // TODO: Lab 5.2 读取持久化的事务状态信息
    global_seq_ = tranc_info_file_.read_uint64(0);
    max_flushed_seq_ = tranc_info_file_.read_uint64(sizeof(uint64_t));
}

// 更新最大已刷盘seq
void TranManager::update_max_flushed_seq(uint64_t tranc_id){
    uint64_t cur = max_flushed_seq_.load();
    // cur 会被更新成最新值
    while (cur < tranc_id && !max_flushed_seq_.compare_exchange_weak(cur, tranc_id)) {}
    // 恢复 WAL 阶段 wal 尚未初始化，此时只需要更新内存中的 max_flushed_seq_，
    // 后续 init_new_wal() 会用该值创建新的 WAL。
    if (wal) {
        wal->reset_max_flushed_seq(max_flushed_seq_.load());
    }
}

// 待flush的事务id数组中增加一个元素
// void TranManager::add_ready_to_flush(uint64_t committed_seq, TransactionState state) {
//     std::unique_lock lock(mutex_);
//     ready_to_flush_[committed_seq] = state;
// }

// 该操作在LSMEngine::flush时被调用
// 将ready_to_flush_中已经flush的事务id移到flushedTrancIds_集合中
// void TranManager::clean_ready_to_flush(uint64_t id) {
//     // 当某个事务 tranc_id 被确认“已经 flush 到持久层”时，把它以及它之前可以一起确认的事务，
//     // 统一推进到 flushedTrancIds_（已落盘集合），并清理中间状态。
//     std::unique_lock lock(mutex_);
//     while (!ready_to_flush_.empty()) {
//         auto it = ready_to_flush_.begin();//取最小的key对应的迭代器
//         if (it->first <= id) {
//             ready_to_flush_.erase(it);
//         } else {
//             break;
//         }
//     }
// }

// 事务id计数器+1
uint64_t TranManager::get_next_global_seq() {
    return global_seq_.fetch_add(1);
}

uint64_t TranManager::get_max_flushed_seq(){
    return max_flushed_seq_.load();
}
// std::set<uint64_t>& TranManager::get_flushed_tranc_ids() {
//     return flushedTrancIds_;
// }

std::shared_ptr<TranContext> TranManager::new_tranc(
    const IsolationLevel& isolation_level) {
    // TODO: Lab 5.2 创建新事务（初始化事务上下文）
    spdlog::debug(
        "TranManager--new_tranc(): Creating new transaction with "
        "isolation level={}",
        static_cast<int>(isolation_level));

    // 获取锁
    std::unique_lock<std::mutex> lock(mutex_);
    // 获得事务id，并创建事务上下文对象
    auto tranc_id = get_next_global_seq();
    // activeTrans_[tranc_id] = std::make_shared<TranContext>(
    //     tranc_id, engine_, shared_from_this(), isolation_level);
    auto new_trancontext = std::make_shared<TranContext>(
        tranc_id, engine_, shared_from_this(), isolation_level);

    spdlog::debug(
        "TranManager--new_tranc(): Created transaction ID={} with "
        "isolation level={}",
        tranc_id, static_cast<int>(isolation_level));

    // return activeTrans_[tranc_id];
    return new_trancontext;
}

std::string TranManager::get_tranc_info_file_path() {
    if (data_dir_.empty()) {
        data_dir_ = "./";
    }
    return data_dir_ + "/tranc_id";
}

std::map<uint64_t, std::vector<Record>> TranManager::check_recover() {
    spdlog::info("TranManager--check_recover(): Starting recovery from WAL");
    // 通过Wal的static方法，先获取所有大于最大已落盘id的事务
    auto wal_records = WAL::recover(data_dir_, max_flushed_seq_);
    
    spdlog::info("TranManager--check_recover(): Recovered {} transactions",
                 wal_records.size());
    return wal_records;
}


// 将Record集合operations写入wal中，并将wal文件落盘
bool TranManager::write_to_wal(const std::vector<Record>& records) {
    spdlog::trace("TranManager--write_to_wal(): Writing {} records to WAL",
                  records.size());
    try {  
        // 将Records数组写入wal中，并刷入wal文件
        wal->log(records, true);
    } catch (const std::exception& e) {
        spdlog::error("TranManager--write_to_wal(): Exception occurred: {}", e.what());
        return false;
    }

    spdlog::trace(
        "TranManager--write_to_wal(): Successfully wrote {} records to WAL",
        records.size());

    return true;
}
}  // namespace tiny_lsm
