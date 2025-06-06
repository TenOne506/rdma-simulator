#include "rdma_pd.h"
#include "rdma_cache.h"
#include <algorithm>

RdmaPD::RdmaPD(uint32_t context_id)
    : context_id_(context_id) {
    // 生成唯一的PD句柄
    static uint32_t next_pd_handle = 1;
    pd_handle_ = next_pd_handle++;
}

RdmaPD::~RdmaPD() {
    // 清理所有资源
    for (auto& mr : memory_regions_) {
        deregister_memory(mr->get_lkey());
    }
    
    for (auto qp_num : queue_pairs_) {
        destroy_qp(qp_num);
    }
}

std::shared_ptr<RdmaMemoryRegion> RdmaPD::register_memory(void* addr, size_t length, uint32_t access_flags) {
    // 创建新的内存区域
    static uint32_t next_lkey = 1;
    static uint32_t next_rkey = 1;
    auto mr = std::make_shared<RdmaMemoryRegion>(addr, length, next_lkey++, next_rkey++, access_flags);
    
    // 将MR添加到PD的MR列表中
    memory_regions_.push_back(mr);
    return mr;
}

bool RdmaPD::deregister_memory(uint32_t lkey) {
    auto it = std::find_if(memory_regions_.begin(), memory_regions_.end(),
                          [lkey](const auto& mr) { return mr->get_lkey() == lkey; });
    
    if (it == memory_regions_.end()) {
        return false;
    }
    
    memory_regions_.erase(it);
    return true;
}

bool RdmaPD::create_qp(uint32_t& qp_num, uint32_t access_flags) {
    // 生成新的QP编号
    static uint32_t next_qp_num = 1;
    qp_num = next_qp_num++;
    
    // 将QP添加到PD的QP列表中
    queue_pairs_.push_back(qp_num);
    
    return true;
}

bool RdmaPD::destroy_qp(uint32_t qp_num) {
    auto it = std::find(queue_pairs_.begin(), queue_pairs_.end(), qp_num);
    
    if (it == queue_pairs_.end()) {
        return false;
    }
    
    queue_pairs_.erase(it);
    return true;
} 