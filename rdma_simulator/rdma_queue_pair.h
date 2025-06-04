#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include <cstddef>
#include <queue>
#include <cstdint>

enum class RdmaOpType {
    SEND,
    RECV,
    READ,
    WRITE,
    COMP_SWAP,
    FETCH_ADD
};

class RdmaQueuePair {
public:
    enum class State { INIT, RTR, RTS, ERROR };
    
    struct WorkRequest {
        uint64_t wr_id;
        RdmaOpType op_type;
        void* local_addr;
        uint32_t lkey;
        void* remote_addr;
        uint32_t rkey;
        size_t length;
    };
    
    RdmaQueuePair(uint32_t qp_num, uint32_t max_send_wr, uint32_t max_recv_wr);
    
    uint32_t qp_num() const;
    State state() const;
    
    void modify_to_rtr();
    void modify_to_rts();
    
    bool post_send(uint64_t wr_id, RdmaOpType op_type, void* local_addr, 
                   uint32_t lkey, void* remote_addr, uint32_t rkey, size_t length);
    bool post_recv(uint64_t wr_id, void* local_addr, uint32_t lkey, size_t length);
    
    bool get_send_request(WorkRequest& wr);
    bool get_recv_request(WorkRequest& wr);

private:
    uint32_t qp_num_;
    uint32_t max_send_wr_;
    uint32_t max_recv_wr_;
    std::queue<WorkRequest> send_queue_;
    std::queue<WorkRequest> recv_queue_;
    State state_;
};

#endif // RDMA_QUEUE_PAIR_H