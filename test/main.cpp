#include <gtest/gtest.h>
#include "../rdma_simulator/rdma_cache.h"
#include "../rdma_simulator/rdma_device.h"
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>

// 测试夹具类
class RdmaCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试用例开始前的设置
        cache = std::make_unique<RdmaCache>(1024 * 1024); // 1MB cache size
    }

    void TearDown() override {
        // 每个测试用例结束后的清理
        cache.reset();
    }

    std::unique_ptr<RdmaCache> cache;
};

// 基本功能测试
TEST_F(RdmaCacheTest, BasicOperations) {
    // 测试数据
    const char test_data[] = "Hello, RDMA Cache!";
    const size_t data_size = strlen(test_data) + 1;
    
    // 测试 put 和 get
    cache->put(RdmaCache::MR, 1, test_data, data_size);
    
    char buffer[256] = {0};
    EXPECT_TRUE(cache->get(RdmaCache::MR, 1, buffer, data_size));
    EXPECT_STREQ(test_data, buffer);
    
    // 测试 contains
    EXPECT_TRUE(cache->contains(RdmaCache::MR, 1));
    EXPECT_FALSE(cache->contains(RdmaCache::MR, 2));
}

// 缓存策略测试
TEST_F(RdmaCacheTest, CachePolicy) {
    // 创建一个小缓存，使用LRU策略
    cache = std::make_unique<RdmaCache>(100, RdmaCache::LRU);
    
    // 填充缓存
    const char data1[] = "Data1";
    const char data2[] = "Data2";
    const char data3[] = "Data3";
    
    cache->put(RdmaCache::MR, 1, data1, strlen(data1) + 1);
    cache->put(RdmaCache::MR, 2, data2, strlen(data2) + 1);
    cache->put(RdmaCache::MR, 3, data3, strlen(data3) + 1);
    
    // 验证缓存统计
    auto stats = cache->get_stats();
    EXPECT_GT(stats.evictions, 0); // 应该有驱逐发生
}

// 高并发测试
TEST_F(RdmaCacheTest, HighConcurrencyTest) {
    const int num_threads = 4;
    const int num_operations = 1000;
    std::atomic<int> success_count{0};
    
    auto worker = [&](int thread_id) {
        for (int i = 0; i < num_operations; ++i) {
            uint32_t key = thread_id * num_operations + i;
            const char* data = "Test Data";
            size_t data_size = strlen(data) + 1;
            
            cache->put(RdmaCache::MR, key, data, data_size);
            
            char buffer[256] = {0};
            if (cache->get(RdmaCache::MR, key, buffer, data_size)) {
                success_count++;
            }
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(success_count, num_threads * num_operations);
}

// 缓存大小影响测试
TEST_F(RdmaCacheTest, CacheSizeImpactTest) {
    const size_t small_size = 100;
    const size_t large_size = 1024 * 1024;
    
    // 测试小缓存
    cache = std::make_unique<RdmaCache>(small_size);
    const char* data = "Test Data";
    size_t data_size = strlen(data) + 1;
    
    // 填充小缓存
    for (int i = 0; i < 10; ++i) {
        cache->put(RdmaCache::MR, i, data, data_size);
    }
    
    auto small_stats = cache->get_stats();
    EXPECT_GT(small_stats.evictions, 0);
    
    // 测试大缓存
    cache = std::make_unique<RdmaCache>(large_size);
    
    // 填充大缓存
    for (int i = 0; i < 1000; ++i) {
        cache->put(RdmaCache::MR, i, data, data_size);
    }
    
    auto large_stats = cache->get_stats();
    EXPECT_LT(large_stats.evictions, small_stats.evictions);
}

// RdmaDevice 测试夹具
class RdmaDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        device = std::make_unique<RdmaDevice>();
    }

    void TearDown() override {
        device.reset();
    }

    std::unique_ptr<RdmaDevice> device;
};

// 设备缓存集成测试
TEST_F(RdmaDeviceTest, CacheIntegration) {
    // 创建测试数据
    const char test_data[] = "Test Data for Device Cache";
    const size_t data_size = strlen(test_data) + 1;
    char* buffer = new char[data_size];
    std::memcpy(buffer, test_data, data_size);
    
    // 注册内存区域
    uint32_t mr_key = device->register_mr(buffer, data_size);
    EXPECT_NE(mr_key, 0);
    
    // 测试缓存操作
    device->cache_put(buffer, test_data, data_size, mr_key);
    
    char read_buffer[256] = {0};
    EXPECT_TRUE(device->cache_get(buffer, read_buffer, data_size));
    EXPECT_STREQ(test_data, read_buffer);
    
    // 清理
    delete[] buffer;
}

// 批量操作测试
TEST_F(RdmaDeviceTest, BatchOperations) {
    // 批量创建QP
    std::vector<uint32_t> qp_nums = device->create_qp_batch(5, 100, 100);
    EXPECT_EQ(qp_nums.size(), 5);
    
    // 批量创建CQ
    std::vector<uint32_t> cq_nums = device->create_cq_batch(5, 100);
    EXPECT_EQ(cq_nums.size(), 5);
    
    // 批量注册MR
    std::vector<std::pair<void*, size_t>> regions;
    for (int i = 0; i < 5; ++i) {
        char* buffer = new char[1024];
        regions.push_back({buffer, 1024});
    }
    
    std::vector<uint32_t> mr_keys = device->register_mr_batch(regions);
    EXPECT_EQ(mr_keys.size(), 5);
    
    // 清理
    for (auto& region : regions) {
        delete[] static_cast<char*>(region.first);
    }
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 