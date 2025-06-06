#include "rdma_queue_pair.h"
#include <stdexcept>

RdmaQueuePair::RdmaQueuePair(uint32_t qp_num, uint32_t max_send_wr, uint32_t max_recv_wr)
    : qp_num_(qp_num),
      state_(QP_STATE_RESET),
      max_send_wr_(max_send_wr),
      max_recv_wr_(max_recv_wr),
      send_queue_(max_send_wr),
      recv_queue_(max_recv_wr),
      is_cached_(false) {
    stats_ = {0, 0, 0, 0, 0, 0, 0, 0, 0};
}

RdmaQueuePair::~RdmaQueuePair() {
    // 清理所有未完成的工作请求
    std::lock_guard<std::mutex> send_lock(send_queue_.mutex);
    std::lock_guard<std::mutex> recv_lock(recv_queue_.mutex);
    std::lock_guard<std::mutex> comp_lock(completion_mutex_);
    
    while (!send_queue_.queue.empty()) {
        send_queue_.queue.pop();
    }
    while (!recv_queue_.queue.empty()) {
        recv_queue_.queue.pop();
    }
    while (!completion_queue_.empty()) {
        completion_queue_.pop();
    }
}

bool RdmaQueuePair::modify_to_init() {
    if (state_ != QP_STATE_RESET) {
        return false;
    }
    state_ = QP_STATE_INIT;
    return true;
}

bool RdmaQueuePair::modify_to_rtr(uint32_t remote_qp_num, uint32_t remote_lid, uint32_t remote_gid) {
    if (state_ != QP_STATE_INIT) {
        return false;
    }
    
    remote_qp_.qp_num = remote_qp_num;
    remote_qp_.lid = remote_lid;
    remote_qp_.gid = remote_gid;
    remote_qp_.valid = true;
    
    state_ = QP_STATE_RTR;
    return true;
}

bool RdmaQueuePair::modify_to_rts() {
    if (state_ != QP_STATE_RTR || !remote_qp_.valid) {
        return false;
    }
    state_ = QP_STATE_RTS;
    return true;
}

bool RdmaQueuePair::post_send(uint64_t wr_id, RdmaOpType op_type, void* local_addr,
                             uint32_t local_key, void* remote_addr, uint32_t remote_key,
                             uint32_t length, bool signaled) {
    if (state_ != QP_STATE_RTS) {
        return false;
    }

    RdmaWorkRequest wr;
    wr.wr_id = wr_id;
    wr.op_type = op_type;
    wr.local_addr = local_addr;
    wr.local_key = local_key;
    wr.remote_addr = remote_addr;
    wr.remote_key = remote_key;
    wr.length = length;
    wr.signaled = signaled;

    if (!validate_work_request(wr)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(send_queue_.mutex);
    if (send_queue_.current_depth >= max_send_wr_) {
        return false;
    }

    send_queue_.queue.push(wr);
    send_queue_.current_depth++;
    update_stats(wr);
    return true;
}

bool RdmaQueuePair::post_recv(uint64_t wr_id, void* local_addr, uint32_t local_key, uint32_t length) {
    if (state_ != QP_STATE_RTS) {
        return false;
    }

    RdmaWorkRequest wr;
    wr.wr_id = wr_id;
    wr.op_type = RdmaOpType::RECV;
    wr.local_addr = local_addr;
    wr.local_key = local_key;
    wr.length = length;

    if (!validate_work_request(wr)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(recv_queue_.mutex);
    if (recv_queue_.current_depth >= max_recv_wr_) {
        return false;
    }

    recv_queue_.queue.push(wr);
    recv_queue_.current_depth++;
    update_stats(wr);
    return true;
}

bool RdmaQueuePair::post_atomic_cmp_swp(uint64_t wr_id, void* local_addr, uint32_t local_key,
                                       void* remote_addr, uint32_t remote_key,
                                       uint64_t compare, uint64_t swap) {
    if (state_ != QP_STATE_RTS) {
        return false;
    }

    RdmaWorkRequest wr;
    wr.wr_id = wr_id;
    wr.op_type = RdmaOpType::ATOMIC_CMP_AND_SWP;
    wr.local_addr = local_addr;
    wr.local_key = local_key;
    wr.remote_addr = remote_addr;
    wr.remote_key = remote_key;
    wr.compare_add = compare;
    wr.swap = swap;

    if (!validate_work_request(wr)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(send_queue_.mutex);
    if (send_queue_.current_depth >= max_send_wr_) {
        return false;
    }

    send_queue_.queue.push(wr);
    send_queue_.current_depth++;
    update_stats(wr);
    return true;
}

bool RdmaQueuePair::post_atomic_fetch_add(uint64_t wr_id, void* local_addr, uint32_t local_key,
                                         void* remote_addr, uint32_t remote_key, uint64_t add) {
    if (state_ != QP_STATE_RTS) {
        return false;
    }

    RdmaWorkRequest wr;
    wr.wr_id = wr_id;
    wr.op_type = RdmaOpType::ATOMIC_FETCH_AND_ADD;
    wr.local_addr = local_addr;
    wr.local_key = local_key;
    wr.remote_addr = remote_addr;
    wr.remote_key = remote_key;
    wr.compare_add = add;

    if (!validate_work_request(wr)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(send_queue_.mutex);
    if (send_queue_.current_depth >= max_send_wr_) {
        return false;
    }

    send_queue_.queue.push(wr);
    send_queue_.current_depth++;
    update_stats(wr);
    return true;
}

bool RdmaQueuePair::get_send_request(RdmaWorkRequest& wr) {
    std::lock_guard<std::mutex> lock(send_queue_.mutex);
    if (send_queue_.queue.empty()) {
        return false;
    }

    wr = send_queue_.queue.front();
    send_queue_.queue.pop();
    send_queue_.current_depth--;
    return true;
}

bool RdmaQueuePair::get_recv_request(RdmaWorkRequest& wr) {
    std::lock_guard<std::mutex> lock(recv_queue_.mutex);
    if (recv_queue_.queue.empty()) {
        return false;
    }

    wr = recv_queue_.queue.front();
    recv_queue_.queue.pop();
    recv_queue_.current_depth--;
    return true;
}

void RdmaQueuePair::post_completion(const RdmaCompletion& comp) {
    std::lock_guard<std::mutex> lock(completion_mutex_);
    completion_queue_.push(comp);
}

RdmaQueuePair::Stats RdmaQueuePair::get_stats() const {
    return stats_;
}

void RdmaQueuePair::update_cache_info() {
    // 更新缓存中的QP信息
    is_cached_ = true;
}

bool RdmaQueuePair::validate_work_request(const RdmaWorkRequest& wr) const {
    // 验证工作请求的有效性
    if (!wr.local_addr || !wr.local_key) {
        return false;
    }

    switch (wr.op_type) {
        case RdmaOpType::SEND:
        case RdmaOpType::RECV:
            return wr.length > 0;
            
        case RdmaOpType::READ:
        case RdmaOpType::WRITE:
            return wr.remote_addr && wr.remote_key && wr.length > 0;
            
        case RdmaOpType::ATOMIC_CMP_AND_SWP:
        case RdmaOpType::ATOMIC_FETCH_AND_ADD:
            return wr.remote_addr && wr.remote_key;
            
        default:
            return false;
    }
}

void RdmaQueuePair::update_stats(const RdmaWorkRequest& wr) {
    switch (wr.op_type) {
        case RdmaOpType::SEND:
            stats_.send_ops++;
            stats_.send_bytes += wr.length;
            break;
            
        case RdmaOpType::RECV:
            stats_.recv_ops++;
            stats_.recv_bytes += wr.length;
            break;
            
        case RdmaOpType::READ:
            stats_.read_ops++;
            stats_.read_bytes += wr.length;
            break;
            
        case RdmaOpType::WRITE:
            stats_.write_ops++;
            stats_.write_bytes += wr.length;
            break;
            
        case RdmaOpType::ATOMIC_CMP_AND_SWP:
        case RdmaOpType::ATOMIC_FETCH_AND_ADD:
            stats_.atomic_ops++;
            break;
    }
}