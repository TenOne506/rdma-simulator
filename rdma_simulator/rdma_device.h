#ifndef RDMA_DEVICE_H
#define RDMA_DEVICE_H

#include <memory>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>
#include <functional>
#include "rdma_cache.h"
#include "rdma_monitor.h"
#include "rdma_queue_pair.h"
#include "rdma_completion_queue.h"
#include "rdma_memory_region.h"

// Forward declarations
struct RdmaWorkRequest;
struct RdmaCompletion;

// 添加QP状态枚举
enum class QPState {
    RESET,
    INIT,
    RTR,
    RTS
};

// 添加连接状态枚举
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

// 添加QP属性结构体
struct QPAttributes {
    uint32_t qp_access_flags;
    uint8_t pkey_index;
    uint8_t port_num;
    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_send_sge;
    uint32_t max_recv_sge;
    uint32_t max_rd_atomic;
    uint32_t max_dest_rd_atomic;
};

// 添加连接消息类型枚举
enum class ConnectionMessageType {
    REQ,
    REP,
    RTR,
    RTS,
    REJ
};

// 添加连接消息结构体
struct ConnectionMessage {
    ConnectionMessageType type;
    uint32_t qp_num;
    QPAttributes qp_attr;
    uint32_t timeout_ms;
    uint32_t remote_mr_key;
    uint64_t remote_addr;
    uint16_t remote_lid;
    uint8_t remote_gid[16];
};

// 添加缺失的类型定义
struct RdmaQPInfo {
    uint32_t qp_num;
    uint32_t qp_access_flags;
    uint8_t pkey_index;
    uint8_t port_num;
    uint8_t gid[16];
};

enum class RdmaControlMsgType {
    CONNECT_REQ,
    CONNECT_RESP,
    READY,
    ERROR
};

struct RdmaControlMsg {
    struct {
        RdmaControlMsgType type;
        uint32_t size;
    } header;
    std::vector<uint8_t> payload;
};

// 添加RdmaControlChannel的完整定义
class RdmaControlChannel {
public:
    RdmaControlChannel() = default;
    virtual ~RdmaControlChannel() = default;

    bool start_server(uint16_t port) {
        // TODO: 实现服务器启动逻辑
        return true;
    }

    bool connect_to_server(const std::string& server_ip, uint16_t port) {
        // TODO: 实现客户端连接逻辑
        return true;
    }

    bool send_connect_request(const RdmaQPInfo& qp_info) {
        // TODO: 实现发送连接请求逻辑
        return true;
    }

    bool send_connect_response(const RdmaQPInfo& qp_info, bool accept) {
        // TODO: 实现发送连接响应逻辑
        return true;
    }

    bool send_ready() {
        // TODO: 实现发送就绪消息逻辑
        return true;
    }

    bool send_error(const std::string& error) {
        // TODO: 实现发送错误消息逻辑
        return true;
    }

    bool receive_message(RdmaControlMsg& msg, uint32_t timeout_ms) {
        // TODO: 实现接收消息逻辑
        return true;
    }
};

/**
 * @brief RDMA设备类，模拟RDMA网卡(RNIC)的功能
 * 
 * 该类负责管理RDMA资源，包括队列对(QP)、完成队列(CQ)和内存区域(MR)。
 * 同时提供网络处理线程和缓存系统，用于优化RDMA操作性能。
 */
class RdmaDevice {
public:
    // 添加连接结构体定义
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
        size_t bytes_sent;
        size_t bytes_received;
        size_t error_count;
        std::unique_ptr<RdmaControlChannel> control_channel;
    };

    // 添加回调函数类型定义
    using ConnectionCallback = std::function<void(uint32_t, ConnectionState)>;
    using DataCallback = std::function<void(uint32_t, const void*, size_t)>;
    using ErrorCallback = std::function<void(uint32_t, const std::string&)>;

    /**
     * @brief 构造函数，初始化RDMA设备
     */
    RdmaDevice();

    /**
     * @brief 析构函数，清理RDMA设备资源
     */
    ~RdmaDevice();
    
    // 基本资源管理函数
    uint32_t create_qp(uint32_t max_send_wr, uint32_t max_recv_wr);
    uint32_t create_cq(uint32_t max_cqe);
    uint32_t register_mr(void* addr, size_t length);
    RdmaQueuePair* get_qp(uint32_t qp_num);
    RdmaCompletionQueue* get_cq(uint32_t cq_num);
    RdmaMemoryRegion* get_mr(uint32_t lkey);

    // 网络线程管理
    void start_network_thread();
    void stop_network_thread();

    // 缓存操作
    void cache_put(void* addr, const void* data, size_t size, uint32_t lkey);
    bool cache_get(void* addr, void* buffer, size_t size);
    Stats get_cache_stats() const;

    // 批量操作
    std::vector<uint32_t> create_qp_batch(size_t count, uint32_t max_send_wr, uint32_t max_recv_wr);
    std::vector<uint32_t> create_cq_batch(size_t count, uint32_t max_cqe);
    std::vector<uint32_t> register_mr_batch(const std::vector<std::pair<void*, size_t>>& regions);
    void destroy_qp_batch(const std::vector<uint32_t>& qp_nums);
    void destroy_cq_batch(const std::vector<uint32_t>& cq_nums);
    void deregister_mr_batch(const std::vector<uint32_t>& mr_keys);

    // 连接管理
    size_t connection_count() const;
    void set_max_connections(size_t max_conn);
    std::vector<Connection> establish_connections(
        size_t count, size_t buffer_size, uint32_t max_send_wr, uint32_t max_recv_wr, uint32_t max_cqe);
    Connection get_connection(uint32_t qp_num) const;
    void close_connection(uint32_t qp_num);
    bool establish_rc_connection(uint32_t qp_num, 
                               const std::string& remote_addr,
                               uint16_t remote_port,
                               const QPAttributes& qp_attr,
                               uint32_t timeout_ms);
    bool establish_rdma_connection(uint32_t qp_num, 
                                 const std::string& remote_addr,
                                 uint16_t remote_port,
                                 uint32_t timeout_ms);
    bool init_qp(uint32_t qp_num, uint32_t cq_num);

    // 数据收发
    void post_send_batch(const std::vector<std::pair<uint32_t, std::vector<char>>>& send_requests);
    void post_recv_batch(const std::vector<uint32_t>& qp_nums);
    bool send_data(uint32_t qp_num, const void* data, size_t length, bool should_wait_for_completion);
    bool receive_data(uint32_t qp_num, void* buffer, size_t buffer_size, 
                     size_t& received_length, uint32_t timeout_ms);

    // 状态管理
    QPState get_qp_state(uint32_t qp_num) const;
    void validate_qp_state(uint32_t qp_num, QPState required_state) const;
    void modify_qp_state(uint32_t qp_num, QPState new_state);
    ConnectionState get_connection_state(uint32_t qp_num) const;
    void update_connection_state(uint32_t qp_num, ConnectionState new_state);

    // 回调注册
    void register_connection_callback(ConnectionCallback callback);
    void register_data_callback(DataCallback callback);
    void register_error_callback(ErrorCallback callback);

    // 通知函数
    void notify_connection_state_change(uint32_t qp_num, ConnectionState state);
    void notify_data_received(uint32_t qp_num, const void* data, size_t length);
    void notify_error(uint32_t qp_num, const std::string& error);

    // 完成事件处理
    bool wait_for_completion(uint32_t qp_num, uint32_t timeout_ms);
    bool poll_completion(uint32_t qp_num, uint32_t timeout_ms);

    // 辅助函数
    uint64_t get_next_wr_id() { return next_wr_id_++; }

    // 监控接口
    const RdmaMonitor& get_monitor() const { return monitor_; }
    RdmaMonitor& get_monitor() { return monitor_; }

private:
    // 互斥锁声明
    mutable std::mutex connections_mutex_;
    mutable std::mutex callbacks_mutex_;
    mutable std::mutex connection_protocol_mutex_;

    // 其他成员变量
    uint32_t next_qp_num_;      ///< 下一个可用的QP编号
    uint32_t next_cq_num_;      ///< 下一个可用的CQ编号
    uint32_t next_mr_key_;      ///< 下一个可用的MR密钥
    std::map<uint32_t, std::unique_ptr<RdmaQueuePair>> qps_;        ///< 队列对映射表，键为QP编号
    std::map<uint32_t, std::unique_ptr<RdmaCompletionQueue>> cqs_;  ///< 完成队列映射表，键为CQ编号
    std::map<uint32_t, std::unique_ptr<RdmaMemoryRegion>> mrs_;     ///< 内存区域映射表，键为本地密钥
    std::thread network_thread_;  ///< 网络处理线程
    bool stop_network_;           ///< 网络线程停止标志
    RdmaCache cache_;  ///< RDMA缓存系统，用于优化内存访问性能
    size_t max_connections_ = 1024; // 默认支持1024连接
    std::atomic<size_t> current_connections_{0};
    RdmaMonitor monitor_;  ///< RDMA操作监控系统

    // 连接相关成员
    std::map<uint32_t, Connection> connections_;
    std::vector<ConnectionCallback> connection_callbacks_;
    std::vector<DataCallback> data_callbacks_;
    std::vector<ErrorCallback> error_callbacks_;
    std::map<uint32_t, QPAttributes> qp_attributes_;
    std::map<uint32_t, uint32_t> connection_retry_counts_;
    std::atomic<uint64_t> next_wr_id_{1};

    // 私有辅助函数
    void network_processing_loop();
};

#endif // RDMA_DEVICE_H