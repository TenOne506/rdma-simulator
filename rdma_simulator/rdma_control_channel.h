#ifndef RDMA_CONTROL_CHANNEL_H
#define RDMA_CONTROL_CHANNEL_H

#include <string>
#include <cstdint>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <atomic>
#include <condition_variable>

// RDMA 控制消息类型
enum class RdmaControlMsgType : uint8_t {
    CONNECT_REQ = 1,
    CONNECT_RESP = 2,
    QP_INFO = 3,
    MR_INFO = 4,
    READY = 5,
    ERROR = 6
};

// RDMA 控制消息头
struct RdmaControlMsgHeader {
    RdmaControlMsgType type;
    uint32_t length;
    uint32_t seq_num;
};

// QP 信息结构
struct RdmaQPInfo {
    uint32_t qp_num;
    uint32_t qp_access_flags;
    uint16_t pkey_index;
    uint8_t port_num;
    uint8_t gid[16];
};

// MR 信息结构
struct RdmaMRInfo {
    uint32_t lkey;
    uint32_t rkey;
    uint64_t addr;
    uint32_t length;
    uint32_t access_flags;
};

// RDMA 控制消息
struct RdmaControlMsg {
    RdmaControlMsgHeader header;
    std::vector<uint8_t> payload;
};

class RdmaControlChannel {
public:
    using MessageCallback = std::function<void(const RdmaControlMsg&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    RdmaControlChannel();
    ~RdmaControlChannel();

    // 服务器端接口
    bool start_server(uint16_t port);
    bool wait_for_client(uint32_t timeout_ms = 5000);
    
    // 客户端接口
    bool connect_to_server(const std::string& server_ip, uint16_t port, uint32_t timeout_ms = 5000);
    
    // 消息发送接口
    bool send_connect_request(const RdmaQPInfo& local_qp_info);
    bool send_connect_response(const RdmaQPInfo& local_qp_info, bool accept);
    bool send_qp_info(const RdmaQPInfo& qp_info);
    bool send_mr_info(const RdmaMRInfo& mr_info);
    bool send_ready();
    bool send_error(const std::string& error_msg);
    
    // 消息接收接口
    bool receive_message(RdmaControlMsg& msg, uint32_t timeout_ms = 5000);
    
    // 回调注册
    void register_message_callback(MessageCallback callback);
    void register_error_callback(ErrorCallback callback);
    
    // 连接状态查询
    bool is_connected() const;
    void close();

private:
    // Socket 相关
    int server_socket_;
    int client_socket_;
    std::atomic<bool> running_;
    std::thread receive_thread_;
    std::mutex socket_mutex_;
    
    // 消息处理
    std::queue<RdmaControlMsg> message_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    // 回调函数
    MessageCallback message_callback_;
    ErrorCallback error_callback_;
    std::mutex callback_mutex_;
    
    // 内部函数
    void receive_loop();
    bool send_message(const RdmaControlMsg& msg);
    void handle_error(const std::string& error);
    void cleanup();
};

#endif // RDMA_CONTROL_CHANNEL_H 