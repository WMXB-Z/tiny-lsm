#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

#include "block/block_cache.h"
#include "config/config.h"
#include "logger/logger.h"
#include "lsm/engine.h"
#include "sst/sst.h"

using namespace ::tiny_lsm;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string pad_num(size_t i, int width) {
    std::ostringstream oss;
    oss << std::setw(width) << std::setfill('0') << i;
    return oss.str();
}

TomlConfig& cfg() {
    return const_cast<TomlConfig&>(TomlConfig::getInstance());
}

size_t sum_sst_bytes(const std::string& dir) {
    size_t total = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        auto name = e.path().filename().string();
        if (name.rfind("sst_", 0) == 0 && e.is_regular_file()) {
            total += e.file_size();
        }
    }
    return total;
}

size_t file_size(const std::string& p) {
    return std::filesystem::exists(p) ? std::filesystem::file_size(p) : 0;
}

// 手动构建一个 SST 文件到 dir/sst_<32位id>.<level>，供 LSMEngine 直接加载
void build_sst_file(const std::string& dir, size_t sst_id, bool has_bloom,
                    size_t num_entries, std::shared_ptr<BlockCache> cache) {
    std::filesystem::create_directories(dir);
    std::string path = dir + "/sst_" + pad_num(sst_id, 32) + ".0";
    SSTBuilder builder(4096, has_bloom);
    for (size_t i = 0; i < num_entries; ++i) {
        builder.add("key" + pad_num(i, 6), "value" + pad_num(i, 6), 0);
    }
    builder.build(sst_id, path, cache);
}

}  // namespace

class ModifyTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (std::filesystem::exists("test_modify_data")) {
            std::filesystem::remove_all("test_modify_data");
        }
        std::filesystem::create_directory("test_modify_data");
    }

    void TearDown() override {
        std::filesystem::remove_all("test_modify_data");
    }
};

// ============ 1. Block 缓存池：引擎级读性能 ============
// -写数据：LSM 写入 2 万个 key 并 flush_all() 落成 SST（写与缓存容量无关）；
// -读：用 LSMEngine 顺序 get 全部 key；
// -before：modify_lsm_block_cache_capacity(1) 构造引擎 → 每次 block 访问都未命中（缓存形同虚设）；
//- after：容量 1024 → 全部命中；

// 使用前：缓存容量=1（每次访问都未命中）；使用后：容量充足（全部命中）
TEST_F(ModifyTest, BlockCacheBenefit) {
    const size_t kEntries = 20000;
    const std::string dir = "test_modify_data/cache_db";

    cfg().modify_lsm_per_mem_size_limit(1024 * 1024);
    cfg().modify_lsm_tol_mem_size_limit(4 * 1024 * 1024);
    cfg().modify_lsm_block_size(4096);
    cfg().modify_lsm_block_cache_k(2);

    // 写数据（与缓存容量无关）
    {
        LSM lsm(dir);
        for (size_t i = 0; i < kEntries; ++i) {
            lsm.put("key" + pad_num(i, 6), "value" + pad_num(i, 6));
        }
        lsm.flush_all();
    }

    auto run_gets = [&](const std::string& label) {
        LSMEngine eng(dir);
        size_t found = 0;
        auto t0 = Clock::now();
        for (size_t i = 0; i < kEntries; ++i) {
            if (eng.get("key" + pad_num(i, 6), 0).has_value()) {
                ++found;
            }
        }
        double ms = ms_since(t0);
        printf("  %-30s: %.3f ms (命中 %zu)\n", label.c_str(), ms, found);
        return ms;
    };

    printf("===== Block Cache 缓存池（引擎读） =====\n");
    printf("entries=%zu\n", kEntries);
    cfg().modify_lsm_block_cache_capacity(1);     // 使用前：缓存形同虚设
    double before_ms = run_gets("before (capacity=1)");
    cfg().modify_lsm_block_cache_capacity(1024);  // 使用后：缓存充足
    double after_ms = run_gets("after  (capacity=1024)");
    printf("speedup: %.2fx\n",
           before_ms / (after_ms > 0.0001 ? after_ms : 0.0001));
    EXPECT_LT(after_ms, before_ms * 0.8);
}

// ============ 2. Bloom 过滤器：引擎级读性能 ============
// BloomFilterBenefit —— 无 bloom vs 有 bloom
// - 用SSTBuilder(block_size, has_bloom) 手动构建同一批 key 的两份 SST（一份带 bloom、一份不带），按 sst_<32位id>.0 命名放进两个数据目录；
// - 用LSMEngine分别加载，查询 10 万次落在 key 区间内但不存在的 key；
// - 小缓存容量(1)保证“无 bloom”路径每次真实读 block；
// - before：无 bloom → 每查询都走 block 二分+读盘；
// - after：有 bloom → 位数组直接排除；

// 使用前：SST 不带 bloom；使用后：SST 带 bloom。查询“不存在”的 key
TEST_F(ModifyTest, BloomFilterBenefit) {
    const size_t kEntries = 20000;
    const size_t kQueries = 100000;
    const std::string dir_wo = "test_modify_data/bloom_off_db";
    const std::string dir_on = "test_modify_data/bloom_on_db";

    // 缓存容量=1：让“无 bloom”路径每次都要真实读 block
    cfg().modify_lsm_block_cache_capacity(1);
    cfg().modify_lsm_block_cache_k(2);
    auto cache = std::make_shared<BlockCache>(1, 2);
    build_sst_file(dir_wo, 1, false, kEntries, cache);  // 使用前：无 bloom
    build_sst_file(dir_on, 1, true, kEntries, cache);   // 使用后：有 bloom

    auto run_queries = [&](const std::string& dir, const std::string& label) {
        LSMEngine eng(dir);
        size_t found = 0;
        auto t0 = Clock::now();
        for (size_t i = 0; i < kQueries; ++i) {
            // 落在 SST key 区间内但不存在的 key
            std::string key = "key" + pad_num(i % 19997, 6) + "a";
            if (eng.get(key, 0).has_value()) {
                ++found;
            }
        }
        double ms = ms_since(t0);
        printf("  %-30s: %.3f ms (误判命中 %zu)\n", label.c_str(), ms, found);
        return std::make_pair(ms, found);
    };

    printf("===== Bloom 过滤器（引擎读） =====\n");
    printf("queries=%zu (不存在的key), entries=%zu\n", kQueries, kEntries);
    auto [ms_wo, found_wo] = run_queries(dir_wo, "before (无 bloom)");
    auto [ms_on, found_on] = run_queries(dir_on, "after  (有 bloom)");
    printf("speedup: %.2fx\n",
           ms_wo / (ms_on > 0.0001 ? ms_on : 0.0001));
    EXPECT_EQ(found_wo, 0);
    EXPECT_EQ(found_on, 0);
    EXPECT_LT(ms_on, ms_wo * 0.9);
}

// ============ 3. VLog (WiscKey 键值分离)：缓存效率收益 ============
// 收益场景：大 value + 小 block 缓存 + 反复全量“扫 key（不取 value）”，
// 这是 WiscKey 的典型场景（小 key 扫描/元数据读取）。
//   内联：每个 block 只装 1 个大 value，索引块数量巨大，缓存远不够，每轮都重新读盘+解码；
//   分离：block 只存 12B 引用，全部索引块能装进缓存，扫 key 时命中率高。
TEST_F(ModifyTest, VLogSeparationBenefit) {
    const size_t kEntries = 5000;
    const size_t kValueSize = 2048;
    const int kRounds = 10;

    cfg().modify_lsm_per_mem_size_limit(1024 * 1024);
    cfg().modify_lsm_tol_mem_size_limit(4 * 1024 * 1024);
    cfg().modify_lsm_block_size(4096);
    cfg().modify_lsm_block_cache_capacity(64);  // 小缓存，放大两者差异
    cfg().modify_lsm_block_cache_k(2);

    auto run_case = [&](size_t threshold, const std::string& dir,
                        const std::string& label) {
        cfg().modify_wisckey_value_threshold(threshold);
        std::filesystem::create_directories(dir);
        double write_ms = 0;
        {
            LSM lsm(dir);
            auto t0 = Clock::now();
            for (size_t i = 0; i < kEntries; ++i) {
                std::string value =
                    std::string(kValueSize, 'v') + pad_num(i, 6);
                lsm.put("key" + pad_num(i, 6), value);
            }
            lsm.flush_all();
            write_ms = ms_since(t0);
        }
        size_t sst_bytes = sum_sst_bytes(dir);
        size_t vlog_bytes = file_size(dir + "/vlog.data");

        // 读：直接打开目录下的 SST，全量扫描 key（不 resolve value），
        // 预热一轮后计时 kRounds 轮，体现 block 缓存效率差异
        double read_ms = 0;
        size_t total = 0;
        {
            auto cache = std::make_shared<BlockCache>(64, 2);
            std::vector<std::shared_ptr<SST>> ssts;
            size_t sst_id = 1;
            for (const auto& e : std::filesystem::directory_iterator(dir)) {
                auto name = e.path().filename().string();
                if (name.rfind("sst_", 0) == 0) {
                    ssts.push_back(SST::open(
                        sst_id++, FileObj::open(e.path().string(), false),
                        cache));
                }
            }
            auto scan_keys = [&]() {
                for (auto& s : ssts) {
                    for (auto it = s->begin(0); it != s->end(); ++it) {
                        (void)it.key();  // 只取 key，不触发 vlog 解析
                    }
                }
            };
            scan_keys();  // 预热
            auto t0 = Clock::now();
            for (int r = 0; r < kRounds; ++r) {
                scan_keys();
                total += kEntries;
            }
            read_ms = ms_since(t0) / kRounds;
        }
        printf("  %-24s: sst=%6zu KB, vlog=%6zu KB, write=%7.2f ms, "
               "scan=%6.2f ms/round\n",
               label.c_str(), sst_bytes / 1024, vlog_bytes / 1024, write_ms,
               read_ms);
        return std::make_tuple(sst_bytes, vlog_bytes, write_ms, read_ms,
                               total);
    };

    printf("===== VLog (WiscKey 键值分离，扫 key 缓存效率) =====\n");
    printf("entries=%zu, value_size=%zu, 总数据=%zu KB, 缓存容量=64 blocks, "
           "rounds=%d\n",
           kEntries, kValueSize, kEntries * kValueSize / 1024, kRounds);
    auto [sst_before, vlog_before, write_before, read_before, total_before] =
        run_case(0, "test_modify_data/vlog_inline", "before (值内联)");
    auto [sst_after, vlog_after, write_after, read_after, total_after] =
        run_case(12, "test_modify_data/vlog_sep", "after  (分离vlog)");

    printf("SST 大小下降: %.2fx\n",
           (double)sst_before / (sst_after > 0 ? sst_after : 1));
    printf("扫 key 加速: %.2fx (分离后每轮全量扫 key 更快)\n",
           read_before / (read_after > 0.0001 ? read_after : 0.0001));
    printf("写耗时: before=%.2f ms, after=%.2f ms (vlog 多一次追加)\n",
           write_before, write_after);
    EXPECT_LT(sst_after, sst_before);
    EXPECT_GT(vlog_after, 0);
    EXPECT_LT(read_after, read_before * 0.5);
    EXPECT_EQ(total_after, total_before);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    init_spdlog_file();
    reset_log_level("off");
    return RUN_ALL_TESTS();
}
