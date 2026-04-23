#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include "config/config.h"
#include "logger/logger.h"
#include "lsm/engine.h"
#include "lsm/level_iterator.h"

using namespace ::tiny_lsm;
// 给每个测试用例提供“统一的初始化环境 + 自动清理机制”
class MyTest : public ::testing::Test {
protected:
    // 每个 TEST_F 开始前都会执行：SetUp()
    void SetUp() override {
        // Create a temporary test directory
        test_dir = "test_lsm_data";
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
        if (!std::filesystem::create_directory(test_dir)) {
            std::cout << "文件创建失败\n";
        }
    }
    //   每个 TEST_F 结束后自动执行TearDown()
    void TearDown() override {
        if (no_clear) {
            return;
        }
        // Clean up test directory
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }

    void setNoClear() { no_clear = true; }

    std::string test_dir;
    bool no_clear = false;
};


// TEST_F(MyTest, testConfig){
//     auto size = TomlConfig::getInstance().getLsmPerMemSizeLimit();
//     std::cout << "测试配置类中的参数: " << size << '\n';
// }

// TEST_F(MyTest, Memtable){
//     LSM lsm(test_dir);
//     for(int i = 0; i < 10; i++){
//             lsm.put("key"+ std::to_string(i), "value" + std::to_string(i));
//     }
// }

// TEST_F(MyTest, Recover) {
//     {
//         LSM lsm(test_dir);
//         lsm.put("xxx  ", "yyy");
//         auto tran_ctx = lsm.begin_tran(IsolationLevel::REPEATABLE_READ);

//         for (int i = 0; i < 100; i++) {
//             std::ostringstream oss_key;
//             std::ostringstream oss_value;
//             oss_key << "key" << std::setw(2) << std::setfill('0') << i;
//             oss_value << "value" << std::setw(2) << std::setfill('0') << i;
//             std::string key = oss_key.str();
//             std::string value = oss_value.str();

//             tran_ctx->put(key, value);
//         }
//         // 提交事务时true表示不会真正写入
//         tran_ctx->commit(true);
//     }  // 析构：触发刷盘/关闭

//     {
//         LSM lsm(test_dir);
//         for (int i = 0; i < 100; i++) {
//             std::ostringstream oss_key;
//             std::ostringstream oss_value;
//             oss_key << "key" << std::setw(2) << std::setfill('0') << i;
//             oss_value << "value" << std::setw(2) << std::setfill('0') << i;
//             std::string key = oss_key.str();
//             std::string value = oss_value.str();
//             EXPECT_EQ(lsm.get(key).value(), value);
//         }
//     }  // 重新打开：触发 recover
// }

TEST_F(MyTest, ConcurrentTransactionsTest) {
    {
        LSM lsm(test_dir);
        std::vector<std::thread> threads;
        auto global_ctx = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
        // 先写一部分数据（但不提交）
        for (int j = 0; j < 10; ++j) {
            global_ctx->put(
                "key" + std::to_string(-1) + "-" + std::to_string(j),
                "value" + std::to_string(j));
        }
        // 启动 40 个线程（核心并发部分）
        for (int i = 0; i < 40; ++i) {
            threads.emplace_back([&, i]() {
                auto ctx = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
                for (int j = 0; j < 1000; ++j) {
                    ctx->put(
                        "key" + std::to_string(i) + "-" + std::to_string(j),
                        "value" + std::to_string(j));
                }
                ctx->commit();
            });
        }
        for (auto& t : threads) t.join();
        lsm.flush_all();
        for (int j = 10; j < 20; ++j) {
            global_ctx->put(
                "key" + std::to_string(-1) + "-" + std::to_string(j),
                "value" + std::to_string(j));
        }
        global_ctx->commit(true);
    }

    // 崩溃恢复后验证

    {
        LSM lsm(test_dir);
        for (int i = 0; i < 40; ++i) {
            for (int j = 0; j < 100; ++j) {
                auto val = lsm.get("key" + std::to_string(i) + "-" +
                                   std::to_string(j));
                EXPECT_TRUE(val.has_value());
                EXPECT_EQ(val.value(), "value" + std::to_string(j));
            }
        }
        for (int j = 0; j < 20; ++j) {
            auto val =
                lsm.get("key" + std::to_string(-1) + "-" + std::to_string(j));
            EXPECT_TRUE(val.has_value());
            EXPECT_EQ(val.value(), "value" + std::to_string(j));
        }
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    init_spdlog_file();
    // 执行所有TEST_F测试函数
    return RUN_ALL_TESTS();
}