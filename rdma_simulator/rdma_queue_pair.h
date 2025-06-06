#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include <queue>
#include <mutex>
#include <atomic>
#include <cstdint>
#include "rdma_memory_region.h"

// RDMA 操作类型
enum class RdmaOpType {
    SEND,
    RECV,
    READ,
    WRITE,
    ATOMIC_CMP_AND_SWP,
    ATOMIC_FETCH_AND_ADD
};

// RDMA 工作请求
struct RdmaWorkRequest {
    uint64_t wr_id;
    RdmaOpType op_type;
    void* local_addr;
    uint32_t local_key;
    void* remote_addr;
    uint32_t remote_key;
    uint32_t length;
    uint64_t compare_add;
    uint64_t swap;
    uint32_t imm_data;
    bool signaled;
};

// RDMA 完成事件
struct RdmaCompletion {
    uint64_t wr_id;
    bool success;
    RdmaOpType op_type;
    uint32_t imm_data;
    uint32_t byte_len;
    uint32_t src_qp;
    uint32_t src_qp_num;
    uint32_t status;  // 0 表示成功，非 0 表示错误码
};

class RdmaQueuePair {
public:
    RdmaQueuePair(uint32_t qp_num, uint32_t max_send_wr, uint32_t max_recv_wr);
    ~RdmaQueuePair();

    // 基本操作
    uint32_t get_qp_num() const { return qp_num_; }
    uint32_t get_state() const { return state_; }
    bool is_ready() const { return state_ == QP_STATE_RTS; }

    // QP 状态转换
    bool modify_to_init();
    bool modify_to_rtr(uint32_t remote_qp_num, uint32_t remote_lid, uint32_t remote_gid);
    bool modify_to_rts();

    // 工作请求操作
    bool post_send(uint64_t wr_id, RdmaOpType op_type, void* local_addr, uint32_t local_key,
                  void* remote_addr, uint32_t remote_key, uint32_t length, bool signaled = true);
    bool post_recv(uint64_t wr_id, void* local_addr, uint32_t local_key, uint32_t length);
    bool post_atomic_cmp_swp(uint64_t wr_id, void* local_addr, uint32_t local_key,
                            void* remote_addr, uint32_t remote_key, uint64_t compare, uint64_t swap);
    bool post_atomic_fetch_add(uint64_t wr_id, void* local_addr, uint32_t local_key,
                              void* remote_addr, uint32_t remote_key, uint64_t add);

    // 完成事件处理
    bool get_send_request(RdmaWorkRequest& wr);
    bool get_recv_request(RdmaWorkRequest& wr);
    void post_completion(const RdmaCompletion& comp);

    // 统计信息
    struct Stats {
        uint64_t send_ops;
        uint64_t recv_ops;
        uint64_t read_ops;
        uint64_t write_ops;
        uint64_t atomic_ops;
        uint64_t send_bytes;
        uint64_t recv_bytes;
        uint64_t read_bytes;
        uint64_t write_bytes;
    };
    Stats get_stats() const;

    // 缓存相关
    void update_cache_info();
    bool is_cached() const { return is_cached_; }
    void set_cached(bool cached) { is_cached_ = cached; }

private:
    // QP 状态
    enum QPState {
        QP_STATE_RESET,
        QP_STATE_INIT,
        QP_STATE_RTR,
        QP_STATE_RTS
    };

    // 工作请求队列
    struct WorkRequestQueue {
        std::queue<RdmaWorkRequest> queue;
        std::mutex mutex;
        uint32_t max_depth;
        uint32_t current_depth;

        WorkRequestQueue(uint32_t max_depth) : max_depth(max_depth), current_depth(0) {}
    };

    uint32_t qp_num_;
    QPState state_;
    uint32_t max_send_wr_;
    uint32_t max_recv_wr_;
    WorkRequestQueue send_queue_;
    WorkRequestQueue recv_queue_;
    std::queue<RdmaCompletion> completion_queue_;
    mutable std::mutex completion_mutex_;
    Stats stats_;
    bool is_cached_;

    // 远程 QP 信息
    struct RemoteQPInfo {
        uint32_t qp_num;
        uint32_t lid;
        uint32_t gid;
        bool valid;

        RemoteQPInfo() : qp_num(0), lid(0), gid(0), valid(false) {}
    } remote_qp_;

    // 辅助函数
    bool validate_work_request(const RdmaWorkRequest& wr) const;
    void update_stats(const RdmaWorkRequest& wr);
};

#endif // RDMA_QUEUE_PAIR_H