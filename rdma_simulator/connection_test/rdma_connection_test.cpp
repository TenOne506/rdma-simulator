#include "rdma_connection_test.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <limits>

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
    
    // 创建并启动工作线程
    size_t num_threads = std::thread::hardware_concurrency();
    if (num_threads > num_connections_) {
        num_threads = num_connections_;
    }
    size_t connections_per_thread = (num_connections_ + num_threads - 1) / num_threads;
    
    std::cout << "Creating " << num_threads << " worker threads" << std::endl;
    
    for (size_t i = 0; i < num_threads; ++i) {
        worker_threads_.emplace_back(&RdmaConnectionTest::worker_thread, this, i);
    }
    
    // 等待测试完成
    {
        std::unique_lock<std::mutex> lock(stats_mutex_);
        test_complete_cv_.wait(lock, [this] { return test_complete_.load(); });
    }
    
    total_stats_.end_time = std::chrono::system_clock::now();
    
    // 等待所有线程完成
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    
    print_stats();
}

void RdmaConnectionTest::worker_thread(size_t thread_id) {
    TestStats thread_stats;
    thread_stats.start_time = std::chrono::system_clock::now();
    
    // 计算每个线程处理的连接范围
    size_t connections_per_thread = (num_connections_ + worker_threads_.size() - 1) / worker_threads_.size();
    size_t start_conn = thread_id * connections_per_thread;
    size_t end_conn = std::min(start_conn + connections_per_thread, num_connections_);
    
    if (start_conn >= end_conn) {
        std::cout << "Thread " << thread_id << " has no connections to handle" << std::endl;
        return;
    }
    
    std::cout << "Thread " << thread_id << " handling connections " 
              << start_conn << " to " << end_conn - 1 << std::endl;
    
    // 为每个连接创建QP和MR
    std::vector<RdmaConnectionManager::Connection> connections;
    try {
        size_t num_conns = end_conn - start_conn;
        std::cout << "Thread " << thread_id << " attempting to establish " << num_conns << " connections" << std::endl;
        
        connections = conn_manager_.establish_connections(
            num_conns,
            buffer_size_,
            16,  // max_send_wr
            16,  // max_recv_wr
            32   // max_cqe
        );
        
        std::cout << "Thread " << thread_id << " successfully established " << connections.size() << " connections" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Thread " << thread_id << " failed to establish connections: " 
                  << e.what() << std::endl;
        return;
    }
    
    thread_stats.total_connections = connections.size();
    
    // 为每个连接执行RDMA操作
    for (const auto& conn : connections) {
        if (stop_test_.load()) {
            break;
        }
        
        try {
            // 建立连接
            if (conn_manager_.establish_rdma_connection(conn.qp_num, "127.0.0.1", 1234)) {
                thread_stats.successful_connections++;
                
                // 执行RDMA操作
                if (perform_rdma_operation(conn.qp_num)) {
                    thread_stats.total_bytes_sent += buffer_size_;
                    thread_stats.total_bytes_received += buffer_size_;
                } else {
                    thread_stats.total_errors++;
                }
            } else {
                thread_stats.failed_connections++;
                thread_stats.total_errors++;
            }
        } catch (const std::exception& e) {
            std::cerr << "Thread " << thread_id << " error with QP " << conn.qp_num 
                      << ": " << e.what() << std::endl;
            thread_stats.total_errors++;
        }
    }
    
    thread_stats.end_time = std::chrono::system_clock::now();
    update_stats(thread_stats);
}

void RdmaConnectionTest::monitor_thread() {
    auto start_time = std::chrono::system_clock::now();
    auto end_time = start_time + std::chrono::seconds(test_duration_seconds_);
    
    while (!stop_test_.load() && std::chrono::system_clock::now() < end_time) {
        print_progress();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    stop_test_ = true;
    test_complete_ = true;
    test_complete_cv_.notify_all();
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
    
    std::cout << "\n\nTest Results:" << std::endl
              << "=============" << std::endl
              << "Duration: " << duration.count() << " seconds" << std::endl
              << "Total Connections: " << total_stats_.total_connections << std::endl
              << "Successful Connections: " << total_stats_.successful_connections << std::endl
              << "Failed Connections: " << total_stats_.failed_connections << std::endl
              << "Total Errors: " << total_stats_.total_errors << std::endl
              << "Total Bytes Sent: " << total_stats_.total_bytes_sent << std::endl
              << "Total Bytes Received: " << total_stats_.total_bytes_received << std::endl
              << "Average Throughput: " 
              << (total_stats_.total_bytes_sent + total_stats_.total_bytes_received) 
                 / duration.count() / 1024 / 1024 
              << " MB/s" << std::endl;
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