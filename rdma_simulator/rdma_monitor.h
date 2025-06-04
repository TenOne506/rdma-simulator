#ifndef RDMA_MONITOR_H
#define RDMA_MONITOR_H

#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdint>

class RdmaMonitor {
public:
    struct ConnectionStats {
        uint64_t send_count = 0;
        uint64_t recv_count = 0;
        uint64_t read_count = 0;
        uint64_t write_count = 0;
        uint64_t error_count = 0;
        uint64_t total_bytes_sent = 0;
        uint64_t total_bytes_received = 0;
        std::chrono::steady_clock::time_point last_send_time;
        std::chrono::steady_clock::time_point last_recv_time;
        
        // 计算发送速率 (bytes/sec)
        double send_rate() const;
        
        // 计算接收速率 (bytes/sec)
        double recv_rate() const;
    };
    
    struct GlobalStats {
        uint64_t total_connections = 0;
        uint64_t active_connections = 0;
        uint64_t total_send_ops = 0;
        uint64_t total_recv_ops = 0;
        uint64_t total_read_ops = 0;
        uint64_t total_write_ops = 0;
        uint64_t total_errors = 0;
        uint64_t total_bytes_sent = 0;
        uint64_t total_bytes_received = 0;
    };
    
    // 记录操作
    void record_send(uint32_t qp_num, size_t size);
    void record_recv(uint32_t qp_num, size_t size);
    void record_read(uint32_t qp_num, size_t size);
    void record_write(uint32_t qp_num, size_t size);
    void record_error(uint32_t qp_num);
    
    // 连接管理
    void add_connection(uint32_t qp_num);
    void remove_connection(uint32_t qp_num);
    
    // 获取统计信息
    ConnectionStats get_stats(uint32_t qp_num) const;
    GlobalStats get_global_stats() const;
    
    // 获取所有连接的统计信息
    std::map<uint32_t, ConnectionStats> get_all_stats() const;
    
    // 打印摘要信息
    void print_summary() const;
    void print_connection_stats(uint32_t qp_num) const;
    
    // 重置统计信息
    void reset_stats(uint32_t qp_num);
    void reset_all_stats();

private:
    mutable std::mutex mutex_;
    std::map<uint32_t, ConnectionStats> stats_;
    GlobalStats global_stats_;
    
    // 更新时间戳并计算速率
    void update_send_timestamp(ConnectionStats& stats, size_t size);
    void update_recv_timestamp(ConnectionStats& stats, size_t size);
};

#endif // RDMA_MONITOR_H