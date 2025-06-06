#include "rdma_connection_test.h"
#include <iostream>
#include <cstdlib>
#include <csignal>

volatile sig_atomic_t g_running = 1;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = 0;
    }
}

int main(int argc, char* argv[]) {
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 默认参数
    size_t num_connections = 4;
    size_t buffer_size = 4096;  // 4KB
    uint32_t test_duration = 60;  // 60秒
    
    // 解析命令行参数
    if (argc > 1) {
        num_connections = std::stoul(argv[1]);
    }
    if (argc > 2) {
        buffer_size = std::stoul(argv[2]);
    }
    if (argc > 3) {
        test_duration = std::stoul(argv[3]);
    }
    
    try {
        std::cout << "RDMA Connection Test" << std::endl
                  << "===================" << std::endl;
        
        RdmaConnectionTest test(num_connections, buffer_size, test_duration);
        test.run();
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
} 