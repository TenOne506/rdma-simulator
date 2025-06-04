#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include "rdma_simulator/rdma_device.h"

void print_cache_stats(const RdmaCache::Stats& stats) {
    std::cout << "\n=== Cache Statistics ===" << std::endl;
    std::cout << "Hits: " << stats.hits << std::endl;
    std::cout << "Misses: " << stats.misses << std::endl;
    std::cout << "Hit Rate: " 
              << (stats.hits + stats.misses > 0 ? 
                  (100.0 * stats.hits) / (stats.hits + stats.misses) : 0)
              << "%" << std::endl;
    std::cout << "Evictions: " << stats.evictions << std::endl;
    std::cout << "========================\n" << std::endl;
}

void demonstrate_cache_operations(RdmaDevice& device) {
    // 准备一些测试数据
    const size_t data_size = 1024;
    std::vector<char> original_data(data_size);
    std::vector<char> read_buffer(data_size);
    
    // 填充测试数据
    for (size_t i = 0; i < data_size; ++i) {
        original_data[i] = 'A' + (i % 26);
    }
    
    // 注册内存区域
    uint32_t mr_key = device.register_mr(original_data.data(), original_data.size());
    
    // 1. 测试基本缓存操作
    std::cout << "Testing basic cache operations..." << std::endl;
    
    // 将数据放入缓存
    device.cache_put(original_data.data(), original_data.data(), original_data.size(), mr_key);
    
    // 从缓存读取数据
    if (device.cache_get(original_data.data(), read_buffer.data(), read_buffer.size())) {
        std::cout << "Cache hit! Data successfully read from cache." << std::endl;
        
        // 验证数据是否正确
        if (memcmp(original_data.data(), read_buffer.data(), data_size) == 0) {
            std::cout << "Data verification: CORRECT" << std::endl;
        } else {
            std::cout << "Data verification: INCORRECT (data mismatch)" << std::endl;
        }
    } else {
        std::cout << "Cache miss! Data not found in cache." << std::endl;
    }
    
    // 2. 测试缓存未命中场景
    std::cout << "\nTesting cache miss scenario..." << std::endl;
    
    // 创建一个不在缓存中的地址
    std::vector<char> uncached_data(data_size, 'Z');
    void* uncached_addr = uncached_data.data();
    
    if (device.cache_get(uncached_addr, read_buffer.data(), read_buffer.size())) {
        std::cout << "Unexpected cache hit!" << std::endl;
    } else {
        std::cout << "Cache miss as expected for uncached address." << std::endl;
    }
    
    // 3. 测试RDMA操作与缓存的交互
    std::cout << "\nTesting RDMA operations with cache..." << std::endl;
    
    // 创建QP和CQ
    uint32_t cq_num = device.create_cq(100);
    uint32_t qp_num = device.create_qp(10, 10);
    
    // 修改QP状态
    if (auto qp = device.get_qp(qp_num)) {
        qp->modify_to_rtr();
        qp->modify_to_rts();
    }
    
    // 准备接收缓冲区
    std::vector<char> recv_buffer(data_size, 0);
    uint32_t recv_mr = device.register_mr(recv_buffer.data(), recv_buffer.size());
    
    // 发布接收请求
    if (auto qp = device.get_qp(qp_num)) {
        qp->post_recv(1, recv_buffer.data(), recv_mr, recv_buffer.size());
    }
    
    // 发布发送请求（使用缓存中的数据）
    if (auto qp = device.get_qp(qp_num)) {
        qp->post_send(2, RdmaOpType::SEND, original_data.data(), mr_key, 
                     recv_buffer.data(), 0, original_data.size());
    }
    
    // 等待发送完成
    if (auto cq = device.get_cq(cq_num)) {
        RdmaCompletion comp;
        while (true) {
            if (cq->poll_completion(comp)) {
                std::cout << "RDMA operation completed: wr_id=" << comp.wr_id 
                          << ", success=" << comp.success 
                          << ", op_type=" << static_cast<int>(comp.op_type) << std::endl;
                if (comp.wr_id == 2) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // 检查接收缓冲区是否被更新
    if (memcmp(original_data.data(), recv_buffer.data(), data_size) == 0) {
        std::cout << "RDMA transfer successful. Data matches." << std::endl;
        
        // 接收缓冲区现在应该被缓存
        if (device.cache_get(recv_buffer.data(), read_buffer.data(), read_buffer.size())) {
            std::cout << "Received data is now in cache." << std::endl;
        }
    } else {
        std::cout << "RDMA transfer failed. Data mismatch." << std::endl;
    }
    
    // 4. 测试缓存替换策略
    std::cout << "\nTesting cache replacement policy..." << std::endl;
    
    // 创建大量数据填满缓存
    const size_t num_entries = 10;
    std::vector<std::vector<char>> large_data(num_entries, std::vector<char>(data_size));
    std::vector<uint32_t> mr_keys(num_entries);
    
    for (size_t i = 0; i < num_entries; ++i) {
        // 填充数据
        std::fill(large_data[i].begin(), large_data[i].end(), '0' + i);
        
        // 注册内存区域
        mr_keys[i] = device.register_mr(large_data[i].data(), large_data[i].size());
        
        // 放入缓存
        device.cache_put(large_data[i].data(), large_data[i].data(), large_data[i].size(), mr_keys[i]);
        
        // 打印当前缓存统计
        auto stats = device.get_cache_stats();
        std::cout << "After adding entry " << i << ": Evictions=" << stats.evictions << std::endl;
    }
    
    // 打印最终缓存统计
    print_cache_stats(device.get_cache_stats());
}

int main() {
    // 初始化RDMA设备，设置64MB缓存
    RdmaDevice device;
    device.start_network_thread();
    
    std::cout << "RDMA Cache Simulator Demonstration\n" << std::endl;
    
    try {
        // 演示缓存操作
        demonstrate_cache_operations(device);
        
        // 打印最终统计信息
        std::cout << "\nFinal Cache Statistics:" << std::endl;
        print_cache_stats(device.get_cache_stats());
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    device.stop_network_thread();
    return 0;
}