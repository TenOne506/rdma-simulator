#ifndef RDMA_COMPLETION_QUEUE_H
#define RDMA_COMPLETION_QUEUE_H

#include "rdma_queue_pair.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstdint>

struct RdmaCompletion {
    uint64_t wr_id;
    bool success;
    RdmaOpType op_type;
};

class RdmaCompletionQueue {
public:
    RdmaCompletionQueue(uint32_t cq_num, uint32_t max_cqe);
    
    bool post_completion(const RdmaCompletion& comp);
    bool poll_completion(RdmaCompletion& comp);
    bool wait_completion(RdmaCompletion& comp);

private:
    uint32_t cq_num_;
    uint32_t max_cqe_;
    std::queue<RdmaCompletion> completions_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

#endif // RDMA_COMPLETION_QUEUE_H