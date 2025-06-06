#include <gtest/gtest.h>
#include "../rdma_cache.h"
#include "../rdma_device.h"
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>

// RdmaDevice 测试夹具
class RdmaDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        device = std::make_unique<RdmaDevice>();
        // 启动网络处理线程
        device->start_network_thread();
    }

    void TearDown() override {
        // 停止网络处理线程
        device->stop_network_thread();
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
    EXPECT_NE(mr_key, 0) << "Failed to register memory region";
    
    // 测试缓存操作
    device->cache_put(buffer, test_data, data_size, mr_key);
    
    // 等待一小段时间确保缓存操作完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 验证缓存是否包含数据
    char read_buffer[256] = {0};
    bool cache_hit = device->cache_get(buffer, read_buffer, data_size);
    
    // 如果缓存未命中，重试几次
    int retry_count = 0;
    while (!cache_hit && retry_count < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cache_hit = device->cache_get(buffer, read_buffer, data_size);
        retry_count++;
    }
    
    EXPECT_TRUE(cache_hit) << "Cache get failed after " << retry_count << " retries";
    if (cache_hit) {
        EXPECT_STREQ(test_data, read_buffer) << "Cached data does not match original data";
    }
    
    // 获取缓存统计信息
    auto stats = device->get_cache_stats();
    std::cout << "Cache Statistics:\n"
              << "Hits: " << stats.hits << "\n"
              << "Misses: " << stats.misses << "\n"
              << "Evictions: " << stats.evictions << "\n";
    
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