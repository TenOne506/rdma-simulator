#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include "rdma_simulator/rdma_connection_manager.h"

constexpr size_t NUM_CONNECTIONS = 1024;
constexpr size_t BUFFER_SIZE = 4096; // 4KB per connection
constexpr size_t THREADS = 16;       // 16个工作线程

std::atomic<uint64_t> total_sends{0};
std::atomic<uint64_t> total_recvs{0};

void worker_thread(RdmaConnectionManager& manager, 
                  const std::vector<uint32_t>& qp_nums,
                  size_t thread_id,
                  size_t ops_per_connection) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(64, BUFFER_SIZE);
    std::uniform_int_distribution<> delay_dist(0, 100);
    
    for (size_t op = 0; op < ops_per_connection; ++op) {
        // 模拟随机延迟
        std::this_thread::sleep_for(std::chrono::microseconds(delay_dist(gen)));
        
        // 为每个QP准备发送数据
        std::vector<std::pair<uint32_t, std::vector<char>>> send_batch;
        for (auto qp_num : qp_nums) {
            size_t msg_size = size_dist(gen);
            std::vector<char> data(msg_size, 'A' + (thread_id % 26));
            send_batch.emplace_back(qp_num, std::move(data));
        }
        
        // 批量发送
        manager.post_send_batch(send_batch);
        total_sends += send_batch.size();
        
        // 批量接收
        manager.post_recv_batch(qp_nums);
        total_recvs += qp_nums.size();
    }
}

int main() {
    RdmaDevice device;
    device.start_network_thread();
    
    // 设置最大连接数
    device.set_max_connections(NUM_CONNECTIONS);
    
    RdmaConnectionManager manager(device);
    
    std::cout << "Establishing " << NUM_CONNECTIONS << " RDMA connections..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    
    // 批量建立连接
    auto connections = manager.establish_connections(NUM_CONNECTIONS, BUFFER_SIZE);
    auto qp_nums = [&]() {
        std::vector<uint32_t> nums;
        for (const auto& conn : connections) {
            nums.push_back(conn.qp_num);
        }
        return nums;
    }();
    
    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "Established " << connections.size() << " connections in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
              << " ms" << std::endl;
    
    if (connections.size() < NUM_CONNECTIONS) {
        std::cerr << "Warning: Only established " << connections.size() 
                  << " out of requested " << NUM_CONNECTIONS << std::endl;
    }
    
    // 分配QP到各线程
    const size_t qps_per_thread = (qp_nums.size() + THREADS - 1) / THREADS;
    std::vector<std::vector<uint32_t>> thread_qps(THREADS);
    
    for (size_t i = 0; i < qp_nums.size(); ++i) {
        thread_qps[i % THREADS].push_back(qp_nums[i]);
    }
    
    // 启动工作线程
    const size_t OPS_PER_CONNECTION = 100; // 每个连接执行100次操作
    std::vector<std::thread> workers;
    
    std::cout << "Starting " << THREADS << " worker threads..." << std::endl;
    start = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < THREADS; ++i) {
        workers.emplace_back(worker_thread, std::ref(manager), 
                           std::cref(thread_qps[i]), i, OPS_PER_CONNECTION);
    }
    
    // 等待工作线程完成
    for (auto& t : workers) {
        t.join();
    }
    
    end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // 打印统计信息
    std::cout << "\n=== Simulation Results ===" << std::endl;
    std::cout << "Total connections: " << connections.size() << std::endl;
    std::cout << "Total send operations: " << total_sends << std::endl;
    std::cout << "Total receive operations: " << total_recvs << std::endl;
    std::cout << "Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "Operations per second: " 
              << (total_sends + total_recvs) * 1000 / duration.count()
              << " ops/sec" << std::endl;
    
    device.stop_network_thread();
    return 0;
}