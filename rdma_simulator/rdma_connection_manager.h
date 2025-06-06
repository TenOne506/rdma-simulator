#ifndef RDMA_CONNECTION_MANAGER_H
#define RDMA_CONNECTION_MANAGER_H

#include <vector>
#include <memory>
#include <unordered_map>
#include <iostream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include "rdma_device.h"

class RdmaConnectionManager {
public:
    enum class QPState {
        RESET,
        INIT,
        RTR,  // Ready to Receive
        RTS   // Ready to Send
    };

    enum class ConnectionState {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        ERROR
    };

    struct Connection {
        uint32_t qp_num;
        uint32_t cq_num;
        uint32_t mr_key;
        void* buffer;
        size_t buffer_size;
        QPState state;
        uint32_t remote_qp_num;
        uint32_t remote_mr_key;
        void* remote_addr;
        uint16_t remote_lid;
        uint8_t remote_gid[16];
        ConnectionState conn_state;
        std::chrono::system_clock::time_point last_activity;
        uint64_t bytes_sent;
        uint64_t bytes_received;
        uint32_t error_count;
    };

    // 通信流程相关的回调函数类型
    using ConnectionCallback = std::function<void(uint32_t qp_num, ConnectionState state)>;
    using DataCallback = std::function<void(uint32_t qp_num, const void* data, size_t length)>;
    using ErrorCallback = std::function<void(uint32_t qp_num, const std::string& error)>;

    RdmaConnectionManager(RdmaDevice& device, size_t max_connections = 1024);
    
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

    // 修改QP状态
    void modify_qp_state(uint32_t qp_num, QPState new_state);
    
    // 设置远程QP信息
    void set_remote_qp_info(uint32_t qp_num, 
                           uint32_t remote_qp_num,
                           uint32_t remote_mr_key,
                           void* remote_addr,
                           uint16_t remote_lid,
                           const uint8_t* remote_gid);

    // 获取QP状态
    QPState get_qp_state(uint32_t qp_num) const;

    // 等待完成事件
    bool poll_completion(uint32_t qp_num, uint32_t timeout_ms = 1000);
    
    // 新的通信流程函数
    bool establish_rdma_connection(uint32_t qp_num, 
                                 const std::string& remote_addr,
                                 uint16_t remote_port,
                                 uint32_t timeout_ms = 5000);

    bool send_data(uint32_t qp_num, 
                  const void* data, 
                  size_t length,
                  bool wait_for_completion = true);

    bool receive_data(uint32_t qp_num,
                     void* buffer,
                     size_t buffer_size,
                     size_t& received_length,
                     uint32_t timeout_ms = 5000);

    bool close_rdma_connection(uint32_t qp_num);

    // 回调函数注册
    void register_connection_callback(ConnectionCallback callback);
    void register_data_callback(DataCallback callback);
    void register_error_callback(ErrorCallback callback);

    // 连接状态查询
    ConnectionState get_connection_state(uint32_t qp_num) const;
    bool is_connection_healthy(uint32_t qp_num) const;
    Connection get_connection_info(uint32_t qp_num) const;

private:
    RdmaDevice& device_;
    size_t max_connections_;
    std::unordered_map<uint32_t, Connection> connections_;
    mutable std::mutex connections_mutex_;
    std::atomic<uint64_t> next_wr_id_{0};

    // 回调函数列表
    std::vector<ConnectionCallback> connection_callbacks_;
    std::vector<DataCallback> data_callbacks_;
    std::vector<ErrorCallback> error_callbacks_;
    mutable std::mutex callbacks_mutex_;

    // 内部辅助函数
    void validate_qp_state(uint32_t qp_num, QPState required_state) const;
    void transition_qp_state(uint32_t qp_num, QPState from_state, QPState to_state);
    void update_connection_state(uint32_t qp_num, ConnectionState new_state);
    void notify_connection_state_change(uint32_t qp_num, ConnectionState state);
    void notify_data_received(uint32_t qp_num, const void* data, size_t length);
    void notify_error(uint32_t qp_num, const std::string& error);
    bool wait_for_completion(uint32_t qp_num, uint32_t timeout_ms);
    bool exchange_connection_info(uint32_t qp_num, const std::string& remote_addr, uint16_t remote_port);
    bool verify_remote_connection(uint32_t qp_num);
};

#endif // RDMA_CONNECTION_MANAGER_H