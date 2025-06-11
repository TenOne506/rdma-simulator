#include "../rdma_device.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>
#include <signal.h>

std::atomic<bool> g_running(true);

void signal_handler(int) {
    g_running = false;
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <mode> [options]" << std::endl;
    std::cout << "Modes:" << std::endl;
    std::cout << "  server <port> <buffer_size>" << std::endl;
    std::cout << "  client <server_ip> <port> <buffer_size>" << std::endl;
}

class RdmaDataTest {
public:
    RdmaDataTest(size_t buffer_size) 
        : device_(1024), buffer_size_(buffer_size) {
        // 注册回调函数
        device_.register_connection_callback(
            [this](uint32_t qp_num, RdmaDevice::ConnectionState state) {
                on_connection_state_change(qp_num, state);
            });
        device_.register_data_callback(
            [this](uint32_t qp_num, const void* data, size_t length) {
                on_data_received(qp_num, data, length);
            });
        device_.register_error_callback(
            [this](uint32_t qp_num, const std::string& error) {
                on_error(qp_num, error);
            });
    }

    bool run_server(uint16_t port) {
        std::cout << "Starting RDMA server on port " << port << std::endl;
        
        // 创建连接
        auto connections = device_.establish_connections(1, buffer_size_);
        if (connections.empty()) {
            std::cerr << "Failed to establish server connection" << std::endl;
            return false;
        }

        server_qp_num_ = connections[0].qp_num;
        std::cout << "Server QP number: " << server_qp_num_ << std::endl;

        // 等待客户端连接
        std::cout << "Waiting for client connection..." << std::endl;
        if (!device_.establish_rdma_connection(server_qp_num_, "0.0.0.0", port)) {
            std::cerr << "Failed to establish RDMA connection" << std::endl;
            return false;
        }

        // 设置远程QP信息
        if (auto qp = device_.get_qp(server_qp_num_)) {
            // 使用默认值，实际应该从连接信息中获取
            if (!qp->modify_to_rtr(server_qp_num_ + 1, 1, 0)) {
                std::cerr << "Failed to set remote QP information" << std::endl;
                return false;
            }
            std::cout << "Set remote QP information for QP " << server_qp_num_ << std::endl;
        }

        // 确保QP状态转换到RTS
        if (auto qp = device_.get_qp(server_qp_num_)) {
            if (!qp->modify_to_rts()) {
                std::cerr << "Failed to transition QP to RTS state" << std::endl;
                return false;
            }
            std::cout << "QP " << server_qp_num_ << " transitioned to RTS state" << std::endl;
        }

        std::cout << "Server ready to receive data..." << std::endl;

        // 接收数据
        std::vector<char> recv_buffer(buffer_size_);
        size_t received_length;
        uint64_t total_bytes_received = 0;
        uint64_t total_messages = 0;
        auto start_time = std::chrono::steady_clock::now();
        
        // 预先提交多个接收请求
        const int num_pre_post = 5;
        for (int i = 0; i < num_pre_post; ++i) {
            if (auto qp = device_.get_qp(server_qp_num_)) {
                uint64_t wr_id = device_.get_next_wr_id();
                if (!qp->post_recv(wr_id, recv_buffer.data(), 0, buffer_size_)) {
                    std::cerr << "Failed to post initial receive request " << i << std::endl;
                    return false;
                }
                std::cout << "Posted initial receive request " << i << " for QP " << server_qp_num_ << std::endl;
            }
        }
        
        while (g_running) {
            // 等待接收数据
            if (device_.receive_data(server_qp_num_, recv_buffer.data(), 
                                   recv_buffer.size(), received_length, 1000)) {
                total_bytes_received += received_length;
                total_messages++;
                
                // 打印接收到的数据（用于调试）
                std::cout << "Received data: ";
                for (size_t i = 0; i < std::min(size_t(10), received_length); ++i) {
                    std::cout << static_cast<int>(recv_buffer[i]) << " ";
                }
                std::cout << "..." << std::endl;
                
                // 验证接收到的数据
                if (!verify_received_data(recv_buffer, received_length)) {
                    std::cerr << "Data verification failed" << std::endl;
                    break;
                }

                std::cout << "Received message " << total_messages 
                          << " (" << received_length << " bytes)" << std::endl;
                
                // 生成并发送响应
                std::vector<char> response = generate_response(recv_buffer, received_length);
                
                // 打印响应数据（用于调试）
                std::cout << "Sending response: ";
                for (size_t i = 0; i < std::min(size_t(10), response.size()); ++i) {
                    std::cout << static_cast<int>(response[i]) << " ";
                }
                std::cout << "..." << std::endl;

                if (!device_.send_data(server_qp_num_, response.data(), 
                                     response.size(), true)) {
                    std::cerr << "Failed to send response" << std::endl;
                    break;
                }

                // 重新提交一个接收请求
                if (auto qp = device_.get_qp(server_qp_num_)) {
                    uint64_t wr_id = device_.get_next_wr_id();
                    if (!qp->post_recv(wr_id, recv_buffer.data(), 0, buffer_size_)) {
                        std::cerr << "Failed to post new receive request" << std::endl;
                        break;
                    }
                    std::cout << "Posted new receive request for QP " << server_qp_num_ << std::endl;
                }

                // 打印统计信息
                auto current_time = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                    current_time - start_time).count();
                if (duration > 0) {
                    double throughput = static_cast<double>(total_bytes_received) / duration;
                    std::cout << "Server Statistics:" << std::endl
                              << "  Total messages: " << total_messages << std::endl
                              << "  Total bytes: " << total_bytes_received << std::endl
                              << "  Throughput: " << throughput << " bytes/sec" << std::endl;
                }
            } else {
                // 检查连接状态
                if (!device_.is_connection_healthy(server_qp_num_)) {
                    std::cerr << "Connection is no longer healthy" << std::endl;
                    break;
                }
            }
        }

        // 清理连接
        device_.close_rdma_connection(server_qp_num_);
        return true;
    }

    bool run_client(const std::string& server_ip, uint16_t port) {
        std::cout << "Starting RDMA client, connecting to " << server_ip << ":" << port << std::endl;
        
        // 创建连接
        auto connections = device_.establish_connections(1, buffer_size_);
        if (connections.empty()) {
            std::cerr << "Failed to establish client connection" << std::endl;
            return false;
        }

        client_qp_num_ = connections[0].qp_num;
        std::cout << "Client QP number: " << client_qp_num_ << std::endl;

        // 连接到服务器
        if (!device_.establish_rdma_connection(client_qp_num_, server_ip, port)) {
            std::cerr << "Failed to connect to server" << std::endl;
            return false;
        }

        std::cout << "Client connected successfully, preparing to send data..." << std::endl;

        // 发送测试数据
        const int num_messages = 10;
        uint64_t total_bytes_sent = 0;
        uint64_t total_bytes_received = 0;
        auto start_time = std::chrono::steady_clock::now();

        for (int i = 0; i < num_messages && g_running; ++i) {
            // 生成测试数据
            std::vector<char> send_buffer = generate_test_data();
            
            // 打印发送的数据（用于调试）
            std::cout << "Sending data: ";
            for (size_t j = 0; j < std::min(size_t(10), send_buffer.size()); ++j) {
                std::cout << static_cast<int>(send_buffer[j]) << " ";
            }
            std::cout << "..." << std::endl;

            std::cout << "Sending message " << (i + 1) << "/" << num_messages 
                      << " (" << send_buffer.size() << " bytes)" << std::endl;
            
            if (!device_.send_data(client_qp_num_, send_buffer.data(), 
                                 send_buffer.size(), true)) {
                std::cerr << "Failed to send test data" << std::endl;
                break;
            }
            total_bytes_sent += send_buffer.size();

            // 接收响应
            std::vector<char> recv_buffer(buffer_size_);
            size_t received_length;
            if (!device_.receive_data(client_qp_num_, recv_buffer.data(), 
                                   recv_buffer.size(), received_length, 5000)) {
                std::cerr << "Failed to receive response" << std::endl;
                break;
            }
            total_bytes_received += received_length;

            // 打印接收到的响应（用于调试）
            std::cout << "Received response: ";
            for (size_t j = 0; j < std::min(size_t(10), received_length); ++j) {
                std::cout << static_cast<int>(recv_buffer[j]) << " ";
            }
            std::cout << "..." << std::endl;

            // 验证响应
            if (!verify_response(send_buffer, recv_buffer, received_length)) {
                std::cerr << "Response verification failed" << std::endl;
                break;
            }

            // 打印统计信息
            auto current_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                current_time - start_time).count();
            if (duration > 0) {
                double send_throughput = static_cast<double>(total_bytes_sent) / duration;
                double recv_throughput = static_cast<double>(total_bytes_received) / duration;
                std::cout << "Client Statistics:" << std::endl
                          << "  Messages sent: " << (i + 1) << "/" << num_messages << std::endl
                          << "  Total bytes sent: " << total_bytes_sent << std::endl
                          << "  Total bytes received: " << total_bytes_received << std::endl
                          << "  Send throughput: " << send_throughput << " bytes/sec" << std::endl
                          << "  Receive throughput: " << recv_throughput << " bytes/sec" << std::endl;
            }

            // 短暂延迟，避免发送太快
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // 清理连接
        device_.close_rdma_connection(client_qp_num_);
        return true;
    }

private:
    void on_connection_state_change(uint32_t qp_num, RdmaDevice::ConnectionState state) {
        std::cout << "Connection state changed for QP " << qp_num << ": " 
                  << static_cast<int>(state) << std::endl;
    }

    void on_data_received(uint32_t qp_num, const void* data, size_t length) {
        std::cout << "Data received for QP " << qp_num << ": " << length << " bytes" << std::endl;
    }

    void on_error(uint32_t qp_num, const std::string& error) {
        std::cerr << "Error for QP " << qp_num << ": " << error << std::endl;
    }

    std::vector<char> generate_test_data() {
        std::vector<char> data(buffer_size_);
        for (size_t i = 0; i < buffer_size_; ++i) {
            data[i] = static_cast<char>(i % 256);
        }
        return data;
    }

    bool verify_received_data(const std::vector<char>& data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            if (data[i] != static_cast<char>(i % 256)) {
                std::cerr << "Data verification failed at position " << i 
                          << ": expected " << static_cast<int>(i % 256)
                          << ", got " << static_cast<int>(data[i]) << std::endl;
                return false;
            }
        }
        return true;
    }

    std::vector<char> generate_response(const std::vector<char>& received_data, size_t length) {
        std::vector<char> response(length);
        for (size_t i = 0; i < length; ++i) {
            response[i] = static_cast<char>(received_data[i] + 1);
        }
        return response;
    }

    bool verify_response(const std::vector<char>& sent_data, 
                        const std::vector<char>& response, 
                        size_t length) {
        for (size_t i = 0; i < length; ++i) {
            if (response[i] != static_cast<char>(sent_data[i] + 1)) {
                std::cerr << "Response verification failed at position " << i 
                          << ": expected " << static_cast<int>(sent_data[i] + 1)
                          << ", got " << static_cast<int>(response[i]) << std::endl;
                return false;
            }
        }
        return true;
    }

    RdmaDevice device_;
    size_t buffer_size_;
    uint32_t server_qp_num_;
    uint32_t client_qp_num_;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::string mode = argv[1];
    if (mode == "server") {
        if (argc != 4) {
            print_usage(argv[0]);
            return 1;
        }
        uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
        size_t buffer_size = std::stoul(argv[3]);
        
        RdmaDataTest test(buffer_size);
        return test.run_server(port) ? 0 : 1;
    }
    else if (mode == "client") {
        if (argc != 5) {
            print_usage(argv[0]);
            return 1;
        }
        std::string server_ip = argv[2];
        uint16_t port = static_cast<uint16_t>(std::stoi(argv[3]));
        size_t buffer_size = std::stoul(argv[4]);
        
        RdmaDataTest test(buffer_size);
        return test.run_client(server_ip, port) ? 0 : 1;
    }
    else {
        print_usage(argv[0]);
        return 1;
    }
} 