#include "rdma_queue_pair.h"

RdmaQueuePair::RdmaQueuePair(uint32_t qp_num, uint32_t max_send_wr, uint32_t max_recv_wr)
    : qp_num_(qp_num), max_send_wr_(max_send_wr), max_recv_wr_(max_recv_wr),
      send_queue_(), recv_queue_(), state_(State::INIT) {}

uint32_t RdmaQueuePair::qp_num() const { return qp_num_; }
RdmaQueuePair::State RdmaQueuePair::state() const { return state_; }

void RdmaQueuePair::modify_to_rtr() { state_ = State::RTR; }
void RdmaQueuePair::modify_to_rts() { state_ = State::RTS; }

bool RdmaQueuePair::post_send(uint64_t wr_id, RdmaOpType op_type, void* local_addr, 
                             uint32_t lkey, void* remote_addr, uint32_t rkey, size_t length) {
    if (send_queue_.size() >= max_send_wr_) {
        return false;
    }
    send_queue_.push({wr_id, op_type, local_addr, lkey, remote_addr, rkey, length});
    return true;
}

bool RdmaQueuePair::post_recv(uint64_t wr_id, void* local_addr, uint32_t lkey, size_t length) {
    if (recv_queue_.size() >= max_recv_wr_) {
        return false;
    }
    recv_queue_.push({wr_id, RdmaOpType::RECV, local_addr, lkey, nullptr, 0, length});
    return true;
}

bool RdmaQueuePair::get_send_request(WorkRequest& wr) {
    if (send_queue_.empty()) return false;
    wr = send_queue_.front();
    send_queue_.pop();
    return true;
}

bool RdmaQueuePair::get_recv_request(WorkRequest& wr) {
    if (recv_queue_.empty()) return false;
    wr = recv_queue_.front();
    recv_queue_.pop();
    return true;
}