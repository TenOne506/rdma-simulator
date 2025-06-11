#include "../rdma_device.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>

class RdmaBasicTest {
public:
    RdmaBasicTest() : device1_(1024), device2_(1024) {
        // 注册回调函数
        device1_.register_connection_callback(
            [this](uint32_t qp_num, RdmaDevice::ConnectionState state) {
                std::cout << "Device 1 QP " << qp_num << " state: " << static_cast<int>(state) << std::endl;
            });
        device2_.register_connection_callback(
            [this](uint32_t qp_num, RdmaDevice::ConnectionState state) {
                std::cout << "Device 2 QP " << qp_num << " state: " << static_cast<int>(state) << std::endl;
            });
    }

    bool run_test() {
        std::cout << "Starting basic RDMA test between two devices" << std::endl;

        // 1. 创建QP和CQ
        std::cout << "Creating QPs and CQs..." << std::endl;
        uint32_t qp1 = device1_.create_qp(16, 16);  // 发送和接收队列大小都是16
        uint32_t cq1 = device1_.create_cq(32);      // 完成队列大小32
        uint32_t qp2 = device2_.create_qp(16, 16);
        uint32_t cq2 = device2_.create_cq(32);

        // 初始化QP
        std::cout << "Initializing QPs..." << std::endl;
        if (!device1_.init_qp(qp1, cq1)) {
            std::cerr << "Failed to initialize QP 1" << std::endl;
            return false;
        }
        if (!device2_.init_qp(qp2, cq2)) {
            std::cerr << "Failed to initialize QP 2" << std::endl;
            return false;
        }

        // 2. 分配和注册内存区域
        std::cout << "Registering memory regions..." << std::endl;
        const size_t buffer_size = 1024;  // 1KB buffer
        void* buffer1 = malloc(buffer_size);
        void* buffer2 = malloc(buffer_size);
        uint32_t mr1 = device1_.register_mr(buffer1, buffer_size);
        uint32_t mr2 = device2_.register_mr(buffer2, buffer_size);

        // 3. 准备测试数据
        std::cout << "Preparing test data..." << std::endl;
        char* data1 = static_cast<char*>(buffer1);
        char* data2 = static_cast<char*>(buffer2);
        for (size_t i = 0; i < buffer_size; ++i) {
            data1[i] = 'A' + (i % 26);  // 填充字母A-Z
            data2[i] = 'a' + (i % 26);  // 填充字母a-z
        }

        // 4. 建立连接
        std::cout << "Establishing connection..." << std::endl;
        
        // 设备2作为服务器，监听在本地端口
        if (!device2_.establish_rdma_connection(qp2, "0.0.0.0", 12345)) {
            std::cerr << "Failed to start server on device 2" << std::endl;
            return false;
        }
        std::cout << "Server started on device 2" << std::endl;

        // 等待服务器启动
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 设备1作为客户端，连接到服务器
        if (!device1_.establish_rdma_connection(qp1, "127.0.0.1", 12345)) {
            std::cerr << "Failed to connect from device 1 to server" << std::endl;
            return false;
        }
        std::cout << "Client connected from device 1" << std::endl;

        // 5. 等待连接建立
        std::cout << "Waiting for connection establishment..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 6. 发送数据
        std::cout << "Sending data..." << std::endl;
        if (!device1_.send_data(qp1, data1, buffer_size)) {
            std::cerr << "Failed to send data from device 1" << std::endl;
            return false;
        }

        if (!device2_.send_data(qp2, data2, buffer_size)) {
            std::cerr << "Failed to send data from device 2" << std::endl;
            return false;
        }

        // 7. 接收数据
        std::cout << "Receiving data..." << std::endl;
        std::vector<char> recv_buffer1(buffer_size);
        std::vector<char> recv_buffer2(buffer_size);
        size_t recv_len1 = 0, recv_len2 = 0;

        if (!device1_.receive_data(qp1, recv_buffer1.data(), buffer_size, recv_len1)) {
            std::cerr << "Failed to receive data on device 1" << std::endl;
            return false;
        }

        if (!device2_.receive_data(qp2, recv_buffer2.data(), buffer_size, recv_len2)) {
            std::cerr << "Failed to receive data on device 2" << std::endl;
            return false;
        }

        // 8. 验证数据
        std::cout << "Verifying data..." << std::endl;
        bool success = true;
        if (recv_len1 != buffer_size || memcmp(recv_buffer1.data(), data2, buffer_size) != 0) {
            std::cerr << "Data verification failed for device 1" << std::endl;
            success = false;
        }
        if (recv_len2 != buffer_size || memcmp(recv_buffer2.data(), data1, buffer_size) != 0) {
            std::cerr << "Data verification failed for device 2" << std::endl;
            success = false;
        }

        // 9. 清理资源
        std::cout << "Cleaning up resources..." << std::endl;
        free(buffer1);
        free(buffer2);

        if (success) {
            std::cout << "Test completed successfully!" << std::endl;
        }
        return success;
    }

private:
    RdmaDevice device1_;
    RdmaDevice device2_;
};

int main() {
    RdmaBasicTest test;
    return test.run_test() ? 0 : 1;
} 