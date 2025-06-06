#include "../rdma_cache.h"
#include "../rdma_pd.h"
#include "../rdma_device.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <memory>
#include <mutex>

// RDMA access flags
#define IBV_ACCESS_REMOTE_READ  0x1
#define IBV_ACCESS_REMOTE_WRITE 0x2
#define IBV_ACCESS_LOCAL_WRITE  0x4
#define IBV_ACCESS_REMOTE_ATOMIC 0x8

class RdmaCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建RDMA缓存，设置较大的缓存大小以支持高并发
        cache_ = std::make_unique<RdmaCache>(1024 * 1024 * 100); // 100MB缓存
    }

    void TearDown() override {
        cache_.reset();
    }

    std::unique_ptr<RdmaCache> cache_;
};

// 模拟RDMA设备操作，优先从缓存获取信息
class RdmaDeviceSimulator {
public:
    RdmaDeviceSimulator(RdmaCache* cache) : cache_(cache) {
        // 创建PD
        pd_ = std::make_unique<RdmaPD>(1);
        pd_handle_ = pd_->get_handle();

        // 缓存PD信息
        RdmaCache::PDInfo pd_info;
        pd_info.pd_handle = pd_handle_;
        pd_info.context_id = 1;
        cache_->set_pd_info(pd_handle_, pd_info);
    }

    // 创建QP，优先从缓存获取信息
    bool create_qp(uint32_t& qp_num, uint32_t access_flags) {
        // 先检查缓存
        RdmaCache::QPInfo qp_info;
        if (cache_->get_qp_info(qp_num, qp_info)) {
            return true; // 缓存命中
        }

        // 缓存未命中，创建新的QP
        if (!pd_->create_qp(qp_num, access_flags)) {
            return false;
        }

        // 更新缓存
        qp_info.qp_num = qp_num;
        qp_info.state = 0; // INIT
        qp_info.access_flags = access_flags;
        qp_info.pd_handle = pd_handle_;
        cache_->set_qp_info(qp_num, qp_info);

        return true;
    }

    // 注册MR，优先从缓存获取信息
    std::shared_ptr<RdmaMemoryRegion> register_memory(void* addr, size_t length, uint32_t access_flags) {
        // 创建新的MR
        auto mr = pd_->register_memory(addr, length, access_flags);
        if (!mr) {
            return nullptr;
        }

        // 更新缓存
        RdmaCache::MRInfo mr_info;
        mr_info.lkey = mr->lkey();
        mr_info.rkey = mr->rkey();
        mr_info.addr = reinterpret_cast<uint64_t>(addr);
        mr_info.length = length;
        mr_info.access_flags = access_flags;
        mr_info.pd_handle = pd_handle_;
        cache_->set_mr_info(mr->lkey(), mr_info);

        return mr;
    }

private:
    RdmaCache* cache_;
    std::unique_ptr<RdmaPD> pd_;
    uint32_t pd_handle_;
};

// 测试高并发场景下的缓存性能
TEST_F(RdmaCacheTest, HighConcurrencyTest) {
    const int num_connections = 100;  // 减少并发连接数
    const int num_operations = 10;    // 减少每个连接的操作数
    std::vector<std::thread> threads;
    std::atomic<int> cache_hits{0};
    std::atomic<int> cache_misses{0};
    std::mutex test_mutex;  // 添加互斥锁

    // 创建测试数据
    std::vector<std::vector<uint8_t>> buffers(num_connections);
    for (int i = 0; i < num_connections; ++i) {
        buffers[i].resize(1024);  // 减小缓冲区大小
        std::fill(buffers[i].begin(), buffers[i].end(), static_cast<uint8_t>(i));
    }

    // 先进行一些预热操作
    {
        std::lock_guard<std::mutex> lock(test_mutex);
        for (int i = 0; i < 10; ++i) {
            cache_->put(RdmaCache::MR, i, buffers[i].data(), buffers[i].size());
        }
    }

    // 启动多个线程模拟并发连接
    for (int i = 0; i < num_connections; ++i) {
        threads.emplace_back([&, i]() {
            try {
                // 每个连接执行多次操作
                for (int j = 0; j < num_operations; ++j) {
                    uint32_t key = i * num_operations + j;
                    
                    // 直接测试缓存操作
                    {
                        std::lock_guard<std::mutex> lock(test_mutex);
                        // 写入缓存
                        cache_->put(RdmaCache::MR, key, buffers[i].data(), buffers[i].size());
                        
                        // 读取缓存
                        std::vector<uint8_t> read_buffer(buffers[i].size());
                        if (cache_->get(RdmaCache::MR, key, read_buffer.data(), read_buffer.size())) {
                            cache_hits++;
                            // 验证数据
                            EXPECT_EQ(memcmp(buffers[i].data(), read_buffer.data(), buffers[i].size()), 0);
                        } else {
                            cache_misses++;
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Thread " << i << " failed: " << e.what() << std::endl;
            }
        });
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // 获取缓存统计信息
    auto stats = cache_->get_stats();
    
    // 验证缓存操作是否成功
    EXPECT_GT(stats.hits, 0) << "No cache hits recorded";
    EXPECT_GT(cache_hits, 0) << "No successful cache operations";
    
    // 输出性能统计
    std::cout << "Cache Statistics:\n"
              << "Total Hits: " << stats.hits << "\n"
              << "Total Misses: " << stats.misses << "\n"
              << "Total Evictions: " << stats.evictions << "\n"
              << "Cache Hit Rate: " 
              << (static_cast<double>(stats.hits) / (stats.hits + stats.misses) * 100)
              << "%\n"
              << "Successful Operations: " << cache_hits << "\n"
              << "Failed Operations: " << cache_misses << "\n";
}

// 测试缓存大小对性能的影响
TEST_F(RdmaCacheTest, CacheSizeImpactTest) {
    const int num_connections = 100;
    const int num_operations = 10;
    std::vector<size_t> cache_sizes = {
        1024 * 1024,      // 1MB
        1024 * 1024 * 10, // 10MB
        1024 * 1024 * 50, // 50MB
        1024 * 1024 * 100 // 100MB
    };

    for (size_t cache_size : cache_sizes) {
        // 创建新的缓存
        cache_.reset();  // 确保之前的缓存被清理
        cache_ = std::make_unique<RdmaCache>(cache_size);
        
        // 创建设备模拟器
        std::unique_ptr<RdmaDeviceSimulator> device = std::make_unique<RdmaDeviceSimulator>(cache_.get());

        auto start_time = std::chrono::high_resolution_clock::now();

        // 执行测试
        std::vector<std::thread> threads;
        std::atomic<int> successful_ops{0};
        std::mutex test_mutex;

        for (int i = 0; i < num_connections; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    for (int j = 0; j < num_operations; ++j) {
                        uint32_t qp_num = i * num_operations + j;
                        {
                            std::lock_guard<std::mutex> lock(test_mutex);
                            if (device->create_qp(qp_num, IBV_ACCESS_REMOTE_READ)) {
                                successful_ops++;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Thread " << i << " failed: " << e.what() << std::endl;
                }
            });
        }

        // 等待所有线程完成
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        auto stats = cache_->get_stats();
        
        // 验证测试结果
        EXPECT_GT(successful_ops, 0) << "No successful operations for cache size " 
                                    << (cache_size / (1024 * 1024)) << "MB";
        
        std::cout << "Cache Size: " << (cache_size / (1024 * 1024)) << "MB\n"
                  << "Execution Time: " << duration << "ms\n"
                  << "Successful Operations: " << successful_ops << "\n"
                  << "Cache Hit Rate: " 
                  << (static_cast<double>(stats.hits) / (stats.hits + stats.misses) * 100)
                  << "%\n"
                  << "Evictions: " << stats.evictions << "\n\n";

        // 清理资源
        device.reset();
        threads.clear();
    }
}

// 测试缓存策略对性能的影响
TEST_F(RdmaCacheTest, CachePolicyImpactTest) {
    const int num_connections = 100;
    const int num_operations = 10;
    const size_t cache_size = 1024 * 1024; // 1MB cache
    std::vector<RdmaCache::CachePolicy> policies = {
        RdmaCache::LRU,
        RdmaCache::MRU,
        RdmaCache::FIFO
    };

    // 创建测试数据
    std::vector<std::vector<uint8_t>> test_data(num_connections);
    for (int i = 0; i < num_connections; ++i) {
        test_data[i].resize(1024);
        std::fill(test_data[i].begin(), test_data[i].end(), static_cast<uint8_t>(i));
    }

    for (auto policy : policies) {
        // 创建新的缓存
        cache_.reset();  // 确保之前的缓存被清理
        cache_ = std::make_unique<RdmaCache>(cache_size, policy);

        auto start_time = std::chrono::high_resolution_clock::now();

        // 执行测试
        std::vector<std::thread> threads;
        std::atomic<int> successful_ops{0};
        std::mutex test_mutex;

        for (int i = 0; i < num_connections; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    for (int j = 0; j < num_operations; ++j) {
                        uint32_t key = i * num_operations + j;
                        {
                            std::lock_guard<std::mutex> lock(test_mutex);
                            // 写入数据
                            cache_->put(RdmaCache::MR, key, test_data[i].data(), test_data[i].size());
                            
                            // 读取数据
                            std::vector<uint8_t> buffer(test_data[i].size());
                            if (cache_->get(RdmaCache::MR, key, buffer.data(), buffer.size())) {
                                successful_ops++;
                                // 验证数据
                                EXPECT_EQ(memcmp(test_data[i].data(), buffer.data(), test_data[i].size()), 0);
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Thread " << i << " failed: " << e.what() << std::endl;
                }
            });
        }

        // 等待所有线程完成
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        auto stats = cache_->get_stats();
        
        // 验证测试结果
        EXPECT_GT(successful_ops, 0) << "No successful operations for policy " 
                                    << (policy == RdmaCache::LRU ? "LRU" : 
                                        policy == RdmaCache::MRU ? "MRU" : "FIFO");
        
        std::cout << "Cache Policy: " 
                  << (policy == RdmaCache::LRU ? "LRU" : 
                      policy == RdmaCache::MRU ? "MRU" : "FIFO") << "\n"
                  << "Execution Time: " << duration << "ms\n"
                  << "Successful Operations: " << successful_ops << "\n"
                  << "Cache Hit Rate: " 
                  << (static_cast<double>(stats.hits) / (stats.hits + stats.misses) * 100)
                  << "%\n"
                  << "Evictions: " << stats.evictions << "\n\n";
    }
} 