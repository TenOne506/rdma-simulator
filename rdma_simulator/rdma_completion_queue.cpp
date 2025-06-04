#include "rdma_completion_queue.h"

RdmaCompletionQueue::RdmaCompletionQueue(uint32_t cq_num, uint32_t max_cqe)
    : cq_num_(cq_num), max_cqe_(max_cqe) {}

bool RdmaCompletionQueue::post_completion(const RdmaCompletion& comp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completions_.size() >= max_cqe_) {
        return false;
    }
    completions_.push(comp);
    cv_.notify_one();
    return true;
}

bool RdmaCompletionQueue::poll_completion(RdmaCompletion& comp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completions_.empty()) {
        return false;
    }
    comp = completions_.front();
    completions_.pop();
    return true;
}

bool RdmaCompletionQueue::wait_completion(RdmaCompletion& comp) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !completions_.empty(); });
    comp = completions_.front();
    completions_.pop();
    return true;
}