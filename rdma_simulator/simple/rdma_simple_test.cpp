#include "../rdma_device.h"
#include "../rdma_connection_manager.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <cstring>
#include <algorithm>

// 简单的测试数据生成函数
std::vector<char> generate_test_data(size_t size) {
    std::vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>(i % 256);
    }
    return data;
}

// 验证测试数据
bool verify_test_data(const std::vector<char>& original, const std::vector<char>& received) {
    if (original.size() != received.size()) {
        return false;
    }
    return std::equal(original.begin(), original.end(), received.begin());
}

// 服务器端函数
void run_server(RdmaDevice& device, size_t buffer_size) {
    std::cout << "Starting RDMA server..." << std::endl;
    
    // 创建连接管理器
    RdmaConnectionManager conn_manager(device, 1);
    
    // 建立连接
    auto connections = conn_manager.establish_connections(1, buffer_size);
    if (connections.empty()) {
        std::cerr << "Failed to establish server connection" << std::endl;
        return;
    }
    
    // 等待客户端连接
    std::cout << "Waiting for client connection..." << std::endl;
    bool connected = conn_manager.establish_rdma_connection(connections[0].qp_num, "127.0.0.1", 1234);
    if (!connected) {
        std::cerr << "Failed to establish connection with client" << std::endl;
        return;
    }
    
    std::cout << "Client connected successfully" << std::endl;
    
    // 接收数据
    std::vector<char> recv_buffer(buffer_size);
    size_t received_length;
    
    if (!conn_manager.receive_data(connections[0].qp_num, recv_buffer.data(), 
                                 recv_buffer.size(), received_length)) {
        std::cerr << "Failed to receive data" << std::endl;
        return;
    }
    
    std::cout << "Received " << received_length << " bytes from client" << std::endl;
    
    // 发送响应
    std::vector<char> response = generate_test_data(buffer_size);
    if (!conn_manager.send_data(connections[0].qp_num, response.data(), 
                              response.size(), true)) {
        std::cerr << "Failed to send response" << std::endl;
        return;
    }
    
    std::cout << "Sent " << response.size() << " bytes response to client" << std::endl;
    
    // 等待一段时间确保数据发送完成
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // 关闭连接
    conn_manager.close_rdma_connection(connections[0].qp_num);
    std::cout << "Server connection closed" << std::endl;
}

// 客户端函数
void run_client(RdmaDevice& device, size_t buffer_size) {
    std::cout << "Starting RDMA client..." << std::endl;
    
    // 创建连接管理器
    RdmaConnectionManager conn_manager(device, 1);
    
    // 建立连接
    auto connections = conn_manager.establish_connections(1, buffer_size);
    if (connections.empty()) {
        std::cerr << "Failed to establish client connection" << std::endl;
        return;
    }
    
    // 连接到服务器
    std::cout << "Connecting to server..." << std::endl;
    bool connected = conn_manager.establish_rdma_connection(connections[0].qp_num, "127.0.0.1", 1234);
    if (!connected) {
        std::cerr << "Failed to connect to server" << std::endl;
        return;
    }
    
    std::cout << "Connected to server successfully" << std::endl;
    
    // 发送数据
    std::vector<char> send_data = generate_test_data(buffer_size);
    if (!conn_manager.send_data(connections[0].qp_num, send_data.data(), 
                              send_data.size(), true)) {
        std::cerr << "Failed to send data" << std::endl;
        return;
    }
    
    std::cout << "Sent " << send_data.size() << " bytes to server" << std::endl;
    
    // 接收响应
    std::vector<char> recv_buffer(buffer_size);
    size_t received_length;
    
    if (!conn_manager.receive_data(connections[0].qp_num, recv_buffer.data(), 
                                 recv_buffer.size(), received_length)) {
        std::cerr << "Failed to receive response" << std::endl;
        return;
    }
    
    std::cout << "Received " << received_length << " bytes response from server" << std::endl;
    
    // 验证数据
    if (verify_test_data(send_data, recv_buffer)) {
        std::cout << "Data verification successful" << std::endl;
    } else {
        std::cout << "Data verification failed" << std::endl;
    }
    
    // 关闭连接
    conn_manager.close_rdma_connection(connections[0].qp_num);
    std::cout << "Client connection closed" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <server|client> <buffer_size>" << std::endl;
        return 1;
    }
    
    std::string mode = argv[1];
    size_t buffer_size = std::stoul(argv[2]);
    
    // 创建RDMA设备
    RdmaDevice device(1);  // 只需要一个连接
    
    if (mode == "server") {
        run_server(device, buffer_size);
    } else if (mode == "client") {
        run_client(device, buffer_size);
    } else {
        std::cerr << "Invalid mode. Use 'server' or 'client'" << std::endl;
        return 1;
    }
    
    return 0;
} 