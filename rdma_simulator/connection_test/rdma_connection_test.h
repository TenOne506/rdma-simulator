#ifndef RDMA_CONNECTION_TEST_H
#define RDMA_CONNECTION_TEST_H

#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <random>
#include <iostream>
#include <iomanip>
#include "../rdma_device.h"
#include "../rdma_connection_manager.h"

class RdmaConnectionTest {
public:
    struct TestStats {
        uint64_t total_connections{0};
        uint64_t successful_connections{0};
        uint64_t failed_connections{0};
        uint64_t total_bytes_sent{0};
        uint64_t total_bytes_received{0};
        uint64_t total_errors{0};
        std::chrono::system_clock::time_point start_time;
        std::chrono::system_clock::time_point end_time;
        
        // 新增性能指标
        std::chrono::milliseconds total_connection_time{0};  // 总连接建立时间
        std::chrono::milliseconds total_operation_time{0};   // 总操作时间
        uint64_t connection_timeouts{0};                     // 连接超时次数
        uint64_t operation_timeouts{0};                      // 操作超时次数
        std::vector<std::chrono::milliseconds> connection_times;  // 每个连接的建立时间
        std::vector<std::chrono::milliseconds> operation_times;   // 每个连接的操作时间
    };

    RdmaConnectionTest(size_t num_connections = 512, 
                      size_t buffer_size = 4096,
                      uint32_t test_duration_seconds = 60);
    ~RdmaConnectionTest();

    void run();
    void print_stats() const;

private:
    void worker_thread(size_t thread_id);
    void monitor_thread();
    void print_progress() const;
    bool perform_rdma_operation(uint32_t qp_num);
    void update_stats(const TestStats& thread_stats);

    RdmaDevice device_;
    RdmaConnectionManager conn_manager_;
    size_t num_connections_;
    size_t buffer_size_;
    uint32_t test_duration_seconds_;
    std::vector<std::thread> worker_threads_;
    std::thread monitor_thread_;
    std::atomic<bool> stop_test_{false};
    TestStats total_stats_;
    mutable std::mutex stats_mutex_;
    std::condition_variable test_complete_cv_;
    std::atomic<bool> test_complete_{false};

    // 测试数据生成
    std::random_device rd_;
    std::mt19937 gen_;
    std::uniform_int_distribution<> data_dist_;
    std::vector<char> generate_test_data(size_t size);
    bool verify_test_data(const std::vector<char>& original, const std::vector<char>& received);
};

#endif // RDMA_CONNECTION_TEST_H 