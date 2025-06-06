#include "rdma_completion_queue.h"
#include <algorithm>
#include <sstream>
#include <chrono>

RdmaCompletionQueue::RdmaCompletionQueue(uint32_t cq_num, uint32_t max_cqe)
    : cq_num_(cq_num), max_cqe_(max_cqe), state_(State::INIT) {
    last_completion_time_ = std::chrono::steady_clock::now();
    start_time_ = last_completion_time_;
}

RdmaCompletionQueue::~RdmaCompletionQueue() {
    set_state(State::DESTROYED);
}

bool RdmaCompletionQueue::post_completion(const RdmaCompletion& comp, Priority priority) {
    if (state_ != State::ACTIVE) {
        last_error_ = "Completion queue is not active";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查队列容量
    if (completions_.size() >= max_cqe_) {
        handle_overflow();
        return false;
    }

    // 根据优先级处理完成事件
    switch (priority) {
        case Priority::HIGH:
            priority_queues_.high.push(comp);
            break;
        case Priority::NORMAL:
            priority_queues_.normal.push(comp);
            break;
        case Priority::LOW:
            priority_queues_.low.push(comp);
            break;
    }

    // 更新统计信息和性能指标
    update_stats(comp, priority);
    update_performance_metrics(comp);
    
    // 通知等待的线程
    cv_.notify_one();
    return true;
}

bool RdmaCompletionQueue::poll_completion(RdmaCompletion& comp, std::chrono::milliseconds timeout) {
    if (state_ != State::ACTIVE) {
        last_error_ = "Completion queue is not active";
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    
    // 使用条件变量等待，带超时
    if (timeout.count() > 0) {
        if (!cv_.wait_for(lock, timeout, [this] { 
            return !completions_.empty() || state_ != State::ACTIVE; 
        })) {
            handle_timeout();
            return false;
        }
    }

    // 处理优先级队列
    if (!process_priority_queues(comp)) {
        return false;
    }

    return true;
}

bool RdmaCompletionQueue::wait_completion(RdmaCompletion& comp, std::chrono::milliseconds timeout) {
    if (state_ != State::ACTIVE) {
        last_error_ = "Completion queue is not active";
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    
    // 等待完成事件，带超时
    if (!cv_.wait_for(lock, timeout, [this] { 
        return !completions_.empty() || state_ != State::ACTIVE; 
    })) {
        handle_timeout();
        return false;
    }

    // 处理优先级队列
    if (!process_priority_queues(comp)) {
        return false;
    }

    return true;
}

void RdmaCompletionQueue::set_state(State new_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = new_state;
    cv_.notify_all();
}

void RdmaCompletionQueue::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!completions_.empty()) {
        completions_.pop();
    }
    while (!priority_queues_.high.empty()) priority_queues_.high.pop();
    while (!priority_queues_.normal.empty()) priority_queues_.normal.pop();
    while (!priority_queues_.low.empty()) priority_queues_.low.pop();
    state_ = State::INIT;
    reset_stats();
    start_time_ = std::chrono::steady_clock::now();
}

RdmaCompletionQueue::Stats RdmaCompletionQueue::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void RdmaCompletionQueue::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
}

std::string RdmaCompletionQueue::get_last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void RdmaCompletionQueue::clear_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
    if (state_ == State::ERROR) {
        state_ = State::ACTIVE;
    }
}

size_t RdmaCompletionQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completions_.size();
}

bool RdmaCompletionQueue::is_full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completions_.size() >= max_cqe_;
}

bool RdmaCompletionQueue::is_empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completions_.empty();
}

void RdmaCompletionQueue::process_completion(const RdmaCompletion& comp) {
    if (comp.status != 0) {
        handle_error_completion(comp);
    }
    update_stats(comp, Priority::NORMAL);
    update_performance_metrics(comp);
}

bool RdmaCompletionQueue::handle_error_completion(const RdmaCompletion& comp) {
    std::stringstream ss;
    ss << "Error completion: op=" << static_cast<int>(comp.op_type)
       << ", status=" << comp.status
       << ", wr_id=" << comp.wr_id;
    last_error_ = ss.str();
    state_ = State::ERROR;
    return false;
}

void RdmaCompletionQueue::update_stats(const RdmaCompletion& comp, Priority priority) {
    stats_.total_completions++;
    stats_.total_ops_processed++;
    
    if (comp.status != 0) {
        stats_.error_completions++;
    }

    switch (priority) {
        case Priority::HIGH:
            stats_.high_priority_events++;
            break;
        case Priority::NORMAL:
            stats_.normal_priority_events++;
            break;
        case Priority::LOW:
            stats_.low_priority_events++;
            break;
    }

    calculate_avg_wait_time();
}

void RdmaCompletionQueue::calculate_avg_wait_time() {
    auto now = std::chrono::steady_clock::now();
    auto wait_time = std::chrono::duration_cast<std::chrono::microseconds>(
        now - last_completion_time_).count();
    
    if (stats_.total_completions > 0) {
        stats_.avg_wait_time = (stats_.avg_wait_time * (stats_.total_completions - 1) + wait_time) 
                              / stats_.total_completions;
    }
    
    last_completion_time_ = now;
}

bool RdmaCompletionQueue::process_priority_queues(RdmaCompletion& comp) {
    // 按优先级顺序处理队列
    if (!priority_queues_.high.empty()) {
        comp = priority_queues_.high.front();
        priority_queues_.high.pop();
        return true;
    }
    if (!priority_queues_.normal.empty()) {
        comp = priority_queues_.normal.front();
        priority_queues_.normal.pop();
        return true;
    }
    if (!priority_queues_.low.empty()) {
        comp = priority_queues_.low.front();
        priority_queues_.low.pop();
        return true;
    }
    return false;
}

void RdmaCompletionQueue::handle_overflow() {
    stats_.overflow_count++;
    std::stringstream ss;
    ss << "Completion queue overflow: size=" << completions_.size()
       << ", capacity=" << max_cqe_;
    last_error_ = ss.str();
}

void RdmaCompletionQueue::handle_timeout() {
    stats_.timeout_count++;
    std::stringstream ss;
    ss << "Completion queue timeout: size=" << completions_.size()
       << ", capacity=" << max_cqe_;
    last_error_ = ss.str();
}

void RdmaCompletionQueue::update_performance_metrics(const RdmaCompletion& comp) {
    stats_.total_bytes_processed += comp.byte_len;
}

double RdmaCompletionQueue::get_error_rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stats_.total_completions == 0) {
        return 0.0;
    }
    return static_cast<double>(stats_.error_completions) / stats_.total_completions;
}

double RdmaCompletionQueue::get_throughput() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    if (duration == 0) {
        return 0.0;
    }
    return static_cast<double>(stats_.total_ops_processed) / duration;
}

double RdmaCompletionQueue::get_bandwidth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    if (duration == 0) {
        return 0.0;
    }
    return static_cast<double>(stats_.total_bytes_processed) / duration;
}