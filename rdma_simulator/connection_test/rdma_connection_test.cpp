#include "rdma_connection_test.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <limits>

// 添加QPState类型别名
using QPState = RdmaDevice::QPState;

RdmaConnectionTest::RdmaConnectionTest(size_t num_connections,
                                     size_t buffer_size,
                                     uint32_t test_duration_seconds)
    : device_(),
      conn_manager_(device_, num_connections),
      num_connections_(num_connections),
      buffer_size_(buffer_size),
      test_duration_seconds_(test_duration_seconds),
      gen_(rd_()),
      data_dist_(0, 255) {
    
    std::cout << "Initializing RDMA Connection Test with:" << std::endl
              << "  Number of connections: " << num_connections << std::endl
              << "  Buffer size: " << buffer_size << " bytes" << std::endl
              << "  Test duration: " << test_duration_seconds << " seconds" << std::endl;
}

RdmaConnectionTest::~RdmaConnectionTest() {
    stop_test_ = true;
    test_complete_ = true;
    test_complete_cv_.notify_all();
    
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void RdmaConnectionTest::run() {
    std::cout << "Starting RDMA Connection Test..." << std::endl;
    
    total_stats_.start_time = std::chrono::system_clock::now();
    
    // 启动监控线程
    monitor_thread_ = std::thread(&RdmaConnectionTest::monitor_thread, this);
    
    // 使用单个工作线程处理所有连接
    worker_thread(0);
    
    // 等待测试完成
    {
        std::unique_lock<std::mutex> lock(stats_mutex_);
        test_complete_cv_.wait(lock, [this] { return test_complete_.load(); });
    }
    
    total_stats_.end_time = std::chrono::system_clock::now();
    
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    
    print_stats();
}

void RdmaConnectionTest::worker_thread(size_t thread_id) {
    TestStats thread_stats;
    thread_stats.start_time = std::chrono::system_clock::now();
    
    std::cout << "Attempting to establish " << num_connections_ << " connections" << std::endl;
    
    // 为所有连接创建QP和MR
    std::vector<RdmaConnectionManager::Connection> connections;
    try {
        auto establish_start = std::chrono::steady_clock::now();
        connections = conn_manager_.establish_connections(
            num_connections_,
            buffer_size_,
            16,  // max_send_wr
            16,  // max_recv_wr
            32   // max_cqe
        );
        auto establish_end = std::chrono::steady_clock::now();
        auto establish_duration = std::chrono::duration_cast<std::chrono::milliseconds>(establish_end - establish_start);
        
        thread_stats.total_connection_time = establish_duration;
        std::cout << "Successfully established " << connections.size() 
                  << " connections in " << establish_duration.count() << "ms" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to establish connections: " << e.what() << std::endl;
        return;
    }
    
    thread_stats.total_connections = connections.size();
    
    // 为每个连接执行RDMA操作
    const size_t BATCH_SIZE = 32;  // 每次处理32个连接
    for (size_t batch_start = 0; batch_start < connections.size(); batch_start += BATCH_SIZE) {
        if (stop_test_.load()) {
            break;
        }
        
        size_t batch_end = std::min(batch_start + BATCH_SIZE, connections.size());
        std::cout << "Processing batch " << (batch_start / BATCH_SIZE + 1) 
                  << " (" << batch_start << "-" << (batch_end - 1) << ")" << std::endl;
        
        // 处理当前批次的连接
        for (size_t i = batch_start; i < batch_end; ++i) {
            const auto& conn = connections[i];
            if (stop_test_.load()) {
                break;
            }
            
            try {
                std::cout << "Processing connection " << i + 1 << "/" << connections.size() 
                          << " (QP " << conn.qp_num << ")" << std::endl;
                
                // 建立连接，添加超时处理
                auto connect_start = std::chrono::steady_clock::now();
                bool connected = false;
                int retry_count = 0;
                const int max_retries = 3;
                const int port = 1234 + (i % 100);  // 使用不同的端口避免竞争
                
                while (!connected && !stop_test_.load() && retry_count < max_retries) {
                    // 确保QP状态正确
                    try {
                        auto state = device_.get_qp_state(conn.qp_num);
                        if (state != QPState::INIT && state != QPState::RTR && state != QPState::RTS) {
                            std::cerr << "QP " << conn.qp_num << " is not in correct state, resetting..." << std::endl;
                            device_.modify_qp_state(conn.qp_num, QPState::RESET);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            device_.modify_qp_state(conn.qp_num, QPState::INIT);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Failed to check QP state: " << e.what() << std::endl;
                        break;
                    }
                    
                    // 尝试建立连接，设置较短的超时时间
                    connected = conn_manager_.establish_rdma_connection(conn.qp_num, "127.0.0.1", port, 2000);
                    
                    if (!connected) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - connect_start);
                        
                        if (elapsed.count() >= 2) { // 2秒超时
                            std::cerr << "Connection timeout for QP " << conn.qp_num 
                                     << " (attempt " << retry_count + 1 << "/" 
                                     << max_retries << ")" << std::endl;
                            thread_stats.connection_timeouts++;
                            retry_count++;
                            connect_start = std::chrono::steady_clock::now(); // 重置计时器
                            
                            // 在重试之前等待一段时间
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            continue;
                        }
                        
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                
                auto connect_end = std::chrono::steady_clock::now();
                auto connect_duration = std::chrono::duration_cast<std::chrono::milliseconds>(connect_end - connect_start);
                thread_stats.connection_times.push_back(connect_duration);
                
                if (!connected) {
                    std::cerr << "Failed to connect QP " << conn.qp_num 
                              << " after " << max_retries << " attempts" << std::endl;
                    thread_stats.failed_connections++;
                    thread_stats.total_errors++;
                    continue;
                }
                
                std::cout << "Successfully connected QP " << conn.qp_num 
                          << " in " << connect_duration.count() << "ms" << std::endl;
                thread_stats.successful_connections++;
                
                // 等待连接稳定
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                
                // 执行RDMA操作，添加超时处理
                auto op_start = std::chrono::steady_clock::now();
                bool operation_success = false;
                retry_count = 0;
                
                while (!operation_success && !stop_test_.load() && retry_count < max_retries) {
                    // 确保QP状态正确
                    try {
                        auto state = device_.get_qp_state(conn.qp_num);
                        if (state != QPState::RTS) {
                            std::cerr << "QP " << conn.qp_num << " is not in RTS state for operation" << std::endl;
                            break;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Failed to check QP state: " << e.what() << std::endl;
                        break;
                    }
                    
                    operation_success = perform_rdma_operation(conn.qp_num);
                    
                    if (!operation_success) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - op_start);
                        
                        if (elapsed.count() >= 2) { // 2秒超时
                            std::cerr << "Operation timeout for QP " << conn.qp_num 
                                     << " (attempt " << retry_count + 1 << "/" 
                                     << max_retries << ")" << std::endl;
                            thread_stats.operation_timeouts++;
                            retry_count++;
                            op_start = std::chrono::steady_clock::now(); // 重置计时器
                            
                            // 在重试之前等待一段时间
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            continue;
                        }
                        
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                
                auto op_end = std::chrono::steady_clock::now();
                auto op_duration = std::chrono::duration_cast<std::chrono::milliseconds>(op_end - op_start);
                thread_stats.operation_times.push_back(op_duration);
                thread_stats.total_operation_time += op_duration;
                
                if (!operation_success) {
                    std::cerr << "Failed RDMA operation on QP " << conn.qp_num 
                              << " after " << max_retries << " attempts" << std::endl;
                    thread_stats.total_errors++;
                } else {
                    std::cout << "Successfully completed RDMA operation on QP " << conn.qp_num 
                              << " in " << op_duration.count() << "ms" << std::endl;
                    thread_stats.total_bytes_sent += buffer_size_;
                    thread_stats.total_bytes_received += buffer_size_;
                }
                
                // 等待操作完成
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                
                // 清理连接
                std::cout << "Closing connection for QP " << conn.qp_num << std::endl;
                conn_manager_.close_rdma_connection(conn.qp_num);
                
                // 等待连接完全关闭
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                
            } catch (const std::exception& e) {
                std::cerr << "Error with QP " << conn.qp_num 
                          << ": " << e.what() << std::endl;
                thread_stats.total_errors++;
                
                // 确保在发生异常时也清理连接
                try {
                    conn_manager_.close_rdma_connection(conn.qp_num);
                } catch (...) {
                    // 忽略清理时的错误
                }
            }
        }
        
        // 每批次完成后等待一段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    thread_stats.end_time = std::chrono::system_clock::now();
    update_stats(thread_stats);
}

void RdmaConnectionTest::monitor_thread() {
    auto start_time = std::chrono::system_clock::now();
    auto end_time = start_time + std::chrono::seconds(test_duration_seconds_);
    
    while (!stop_test_.load() && std::chrono::system_clock::now() < end_time) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (total_stats_.total_connections > 0) {
                print_progress();
            }
        }
        
        // 检查是否所有连接都已完成
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (total_stats_.successful_connections + total_stats_.failed_connections >= total_stats_.total_connections) {
                std::cout << "\nAll connections have completed processing." << std::endl;
                stop_test_ = true;
                break;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    stop_test_ = true;
    test_complete_ = true;
    test_complete_cv_.notify_all();
    
    // 打印最终统计信息
    print_stats();
}

void RdmaConnectionTest::print_progress() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - total_stats_.start_time);
    auto remaining = std::chrono::seconds(test_duration_seconds_) - elapsed;
    
    std::cout << "\rProgress: " << elapsed.count() << "/" << test_duration_seconds_ 
              << " seconds (" << (elapsed.count() * 100 / test_duration_seconds_) << "%)"
              << " | Connections: " << total_stats_.successful_connections << "/" 
              << total_stats_.total_connections
              << " | Errors: " << total_stats_.total_errors
              << " | Bytes: " << total_stats_.total_bytes_sent
              << " | Remaining: " << remaining.count() << "s    " << std::flush;
}

void RdmaConnectionTest::print_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        total_stats_.end_time - total_stats_.start_time);
    
    // 计算平均连接时间和操作时间
    double avg_connection_time = 0.0;
    double avg_operation_time = 0.0;
    if (!total_stats_.connection_times.empty()) {
        avg_connection_time = std::accumulate(total_stats_.connection_times.begin(),
                                            total_stats_.connection_times.end(),
                                            std::chrono::milliseconds(0)).count() / 
                            static_cast<double>(total_stats_.connection_times.size());
    }
    if (!total_stats_.operation_times.empty()) {
        avg_operation_time = std::accumulate(total_stats_.operation_times.begin(),
                                           total_stats_.operation_times.end(),
                                           std::chrono::milliseconds(0)).count() / 
                           static_cast<double>(total_stats_.operation_times.size());
    }
    
    // 计算吞吐量
    double total_throughput = (total_stats_.total_bytes_sent + total_stats_.total_bytes_received) 
                            / duration.count() / 1024.0 / 1024.0; // MB/s
    
    std::cout << "\n\nRDMA Connection Test Results" << std::endl
              << "===========================" << std::endl
              << "Test Duration: " << duration.count() << " seconds" << std::endl
              << std::endl
              << "Connection Statistics:" << std::endl
              << "  Total Connections Attempted: " << total_stats_.total_connections << std::endl
              << "  Successful Connections: " << total_stats_.successful_connections << std::endl
              << "  Failed Connections: " << total_stats_.failed_connections << std::endl
              << "  Connection Success Rate: " 
              << (total_stats_.total_connections > 0 ? 
                  (total_stats_.successful_connections * 100.0 / total_stats_.total_connections) : 0)
              << "%" << std::endl
              << std::endl
              << "Performance Metrics:" << std::endl
              << "  Average Connection Time: " << avg_connection_time << " ms" << std::endl
              << "  Average Operation Time: " << avg_operation_time << " ms" << std::endl
              << "  Total Connection Timeouts: " << total_stats_.connection_timeouts << std::endl
              << "  Total Operation Timeouts: " << total_stats_.operation_timeouts << std::endl
              << std::endl
              << "Data Transfer:" << std::endl
              << "  Total Bytes Sent: " << total_stats_.total_bytes_sent << std::endl
              << "  Total Bytes Received: " << total_stats_.total_bytes_received << std::endl
              << "  Average Throughput: " << total_throughput << " MB/s" << std::endl
              << std::endl
              << "Error Statistics:" << std::endl
              << "  Total Errors: " << total_stats_.total_errors << std::endl
              << "  Error Rate: " 
              << (total_stats_.total_connections > 0 ? 
                  (total_stats_.total_errors * 100.0 / total_stats_.total_connections) : 0)
              << "%" << std::endl;
}

bool RdmaConnectionTest::perform_rdma_operation(uint32_t qp_num) {
    // 生成测试数据
    auto send_data = generate_test_data(buffer_size_);
    std::vector<char> recv_data(buffer_size_);
    size_t received_length;
    
    // 发送数据
    if (!conn_manager_.send_data(qp_num, send_data.data(), send_data.size(), true)) {
        return false;
    }
    
    // 接收数据
    if (!conn_manager_.receive_data(qp_num, recv_data.data(), recv_data.size(), received_length)) {
        return false;
    }
    
    // 验证数据
    return verify_test_data(send_data, recv_data);
}

void RdmaConnectionTest::update_stats(const TestStats& thread_stats) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    total_stats_.total_connections += thread_stats.total_connections;
    total_stats_.successful_connections += thread_stats.successful_connections;
    total_stats_.failed_connections += thread_stats.failed_connections;
    total_stats_.total_bytes_sent += thread_stats.total_bytes_sent;
    total_stats_.total_bytes_received += thread_stats.total_bytes_received;
    total_stats_.total_errors += thread_stats.total_errors;
}

std::vector<char> RdmaConnectionTest::generate_test_data(size_t size) {
    std::vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>(data_dist_(gen_));
    }
    return data;
}

bool RdmaConnectionTest::verify_test_data(const std::vector<char>& original, 
                                        const std::vector<char>& received) {
    if (original.size() != received.size()) {
        return false;
    }
    return std::equal(original.begin(), original.end(), received.begin());
} 