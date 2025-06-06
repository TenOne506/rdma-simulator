#ifndef RDMA_COMPLETION_QUEUE_H
#define RDMA_COMPLETION_QUEUE_H

#include "rdma_queue_pair.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <string>
#include <unordered_map>

class RdmaCompletionQueue {
public:
    // 完成队列状态
    enum class State {
        INIT,       // 初始化状态
        ACTIVE,     // 活动状态
        ERROR,      // 错误状态
        DESTROYED   // 已销毁状态
    };

    // 完成事件优先级
    enum class Priority {
        HIGH,       // 高优先级（如错误事件）
        NORMAL,     // 普通优先级（如正常完成）
        LOW         // 低优先级（如异步通知）
    };

    // 统计信息
    struct Stats {
        uint64_t total_completions{0};    // 总完成事件数
        uint64_t error_completions{0};    // 错误事件数
        uint64_t high_priority_events{0}; // 高优先级事件数
        uint64_t normal_priority_events{0};// 普通优先级事件数
        uint64_t low_priority_events{0};  // 低优先级事件数
        uint64_t overflow_count{0};       // 队列溢出次数
        uint64_t timeout_count{0};        // 超时次数
        double avg_wait_time{0.0};        // 平均等待时间
        uint64_t total_bytes_processed{0};// 总处理字节数
        uint64_t total_ops_processed{0};  // 总处理操作数
    };

    RdmaCompletionQueue(uint32_t cq_num, uint32_t max_cqe);
    ~RdmaCompletionQueue();

    // 基本操作
    bool post_completion(const RdmaCompletion& comp, Priority priority = Priority::NORMAL);
    bool poll_completion(RdmaCompletion& comp, std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
    bool wait_completion(RdmaCompletion& comp, std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

    // 状态管理
    State get_state() const { return state_; }
    bool is_active() const { return state_ == State::ACTIVE; }
    void set_state(State new_state);
    void reset();

    // 统计信息
    Stats get_stats() const;
    void reset_stats();

    // 错误处理
    bool has_error() const { return state_ == State::ERROR; }
    std::string get_last_error() const;
    void clear_error();

    // 容量管理
    size_t size() const;
    size_t capacity() const { return max_cqe_; }
    bool is_full() const;
    bool is_empty() const;

    // 完成事件处理
    void process_completion(const RdmaCompletion& comp);
    bool handle_error_completion(const RdmaCompletion& comp);

    // 性能监控
    double get_avg_wait_time() const { return stats_.avg_wait_time; }
    double get_error_rate() const;
    double get_throughput() const;  // 每秒处理的操作数
    double get_bandwidth() const;   // 每秒处理的字节数

private:
    uint32_t cq_num_;
    uint32_t max_cqe_;
    std::atomic<State> state_;
    std::queue<RdmaCompletion> completions_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string last_error_;
    Stats stats_;
    std::chrono::steady_clock::time_point last_completion_time_;
    std::chrono::steady_clock::time_point start_time_;

    // 优先级队列
    struct PriorityQueue {
        std::queue<RdmaCompletion> high;
        std::queue<RdmaCompletion> normal;
        std::queue<RdmaCompletion> low;
    } priority_queues_;

    // 辅助函数
    void update_stats(const RdmaCompletion& comp, Priority priority);
    void calculate_avg_wait_time();
    bool process_priority_queues(RdmaCompletion& comp);
    void handle_overflow();
    void handle_timeout();
    void update_performance_metrics(const RdmaCompletion& comp);
};

#endif // RDMA_COMPLETION_QUEUE_H