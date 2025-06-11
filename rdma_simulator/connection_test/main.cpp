#include "rdma_connection_test.h"
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>

volatile sig_atomic_t g_running = 1;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nReceived signal " << signal << ", stopping test..." << std::endl;
        g_running = 0;
    }
}

int main(int argc, char* argv[]) {
    // 设置信号处理
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        std::cerr << "Failed to set up SIGINT handler" << std::endl;
        return 1;
    }
    if (sigaction(SIGTERM, &sa, nullptr) == -1) {
        std::cerr << "Failed to set up SIGTERM handler" << std::endl;
        return 1;
    }
    
    // 默认参数
    size_t num_connections = 512;
    size_t buffer_size = 4096;  // 4KB
    uint32_t test_duration = 60;  // 60秒
    
    // 解析命令行参数
    try {
        if (argc > 1) {
            num_connections = std::stoul(argv[1]);
        }
        if (argc > 2) {
            buffer_size = std::stoul(argv[2]);
        }
        if (argc > 3) {
            test_duration = std::stoul(argv[3]);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing command line arguments: " << e.what() << std::endl;
        return 1;
    }
    
    try {
        std::cout << "RDMA Connection Test" << std::endl
                  << "===================" << std::endl
                  << "Number of connections: " << num_connections << std::endl
                  << "Buffer size: " << buffer_size << " bytes" << std::endl
                  << "Test duration: " << test_duration << " seconds" << std::endl
                  << "Press Ctrl+C to stop the test" << std::endl
                  << std::endl;
        
        RdmaConnectionTest test(num_connections, buffer_size, test_duration);
        
        // 启动测试
        auto start_time = std::chrono::steady_clock::now();
        test.run();
        auto end_time = std::chrono::steady_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);
        std::cout << "\nTest completed in " << duration.count() << " seconds" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred" << std::endl;
        return 1;
    }
} 