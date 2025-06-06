#ifndef RDMA_PD_H
#define RDMA_PD_H

#include <cstdint>
#include <vector>
#include <memory>
#include "rdma_memory_region.h"

class RdmaPD {
public:
    RdmaPD(uint32_t context_id);
    ~RdmaPD();

    // 获取PD句柄
    uint32_t get_handle() const { return pd_handle_; }
    
    // 获取上下文ID
    uint32_t get_context_id() const { return context_id_; }
    
    // 注册内存区域
    std::shared_ptr<RdmaMemoryRegion> register_memory(void* addr, size_t length, uint32_t access_flags);
    
    // 注销内存区域
    bool deregister_memory(uint32_t lkey);
    
    // 创建QP
    bool create_qp(uint32_t& qp_num, uint32_t access_flags);
    
    // 销毁QP
    bool destroy_qp(uint32_t qp_num);
    
    // 获取PD中的所有MR
    const std::vector<std::shared_ptr<RdmaMemoryRegion>>& get_memory_regions() const {
        return memory_regions_;
    }
    
    // 获取PD中的所有QP
    const std::vector<uint32_t>& get_queue_pairs() const {
        return queue_pairs_;
    }

private:
    uint32_t pd_handle_;         // PD句柄
    uint32_t context_id_;        // 上下文ID
    std::vector<std::shared_ptr<RdmaMemoryRegion>> memory_regions_;  // 注册的内存区域
    std::vector<uint32_t> queue_pairs_;  // 创建的QP列表
};

#endif // RDMA_PD_H 