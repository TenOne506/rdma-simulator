#ifndef RDMA_CONNECTION_MANAGER_H
#define RDMA_CONNECTION_MANAGER_H

#include <vector>
#include <memory>
#include <unordered_map>
#include <iostream> // 添加iostream头文件
#include "rdma_device.h"

class RdmaConnectionManager {
public:
    struct Connection {
        uint32_t qp_num;
        uint32_t cq_num;
        uint32_t mr_key;
        void* buffer;
        size_t buffer_size;
    };
    RdmaConnectionManager(RdmaDevice& device, size_t max_connections = 1024);
    // 修改构造函数，确保正确设置max_connections_
    // RdmaConnectionManager(RdmaDevice& device, size_t max_connections = 1024)
    //     : device_(device), max_connections_(max_connections) {
    //     std::cout << "Initializing RdmaConnectionManager with max_connections=" 
    //               << max_connections << std::endl;
    // }
    
    // 批量建立连接
    std::vector<Connection> establish_connections(size_t count, 
                                                size_t buffer_size,
                                                uint32_t max_send_wr = 16,
                                                uint32_t max_recv_wr = 16,
                                                uint32_t max_cqe = 32);
    
    // 获取连接
    Connection get_connection(uint32_t qp_num) const;
    
    // 关闭连接
    void close_connection(uint32_t qp_num);
    
    // 批量发送数据
    void post_send_batch(const std::vector<std::pair<uint32_t, std::vector<char>>>& send_requests);
    
    // 批量接收数据
    void post_recv_batch(const std::vector<uint32_t>& qp_nums);
    
    // 获取活跃连接数
    size_t active_connections() const;
    
private:
    RdmaDevice& device_;
    size_t max_connections_;
    std::unordered_map<uint32_t, Connection> connections_;
    mutable std::mutex connections_mutex_;
};

#endif // RDMA_CONNECTION_MANAGER_H