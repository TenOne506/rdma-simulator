#include "rdma_device.h"
#include "rdma_cache.h"
#include <iostream>
#include <random>
#include <chrono>

// RDMA access flags
#define IBV_ACCESS_REMOTE_READ  0x1
#define IBV_ACCESS_REMOTE_WRITE 0x2
#define IBV_ACCESS_LOCAL_WRITE  0x4
#define IBV_ACCESS_REMOTE_ATOMIC 0x8

RdmaDevice::RdmaDevice() 
    : next_qp_num_(1), next_cq_num_(1), next_mr_key_(1), 
    stop_network_(false),
    cache_(1024 * 1024 * 64) {} // 64MB cache

RdmaDevice::~RdmaDevice() {
    stop_network_thread();
}

uint32_t RdmaDevice::create_qp(uint32_t max_send_wr, uint32_t max_recv_wr) {
    uint32_t qp_num = next_qp_num_++;
    
    // 先检查缓存
    RdmaCache::QPInfo qp_info;
    if (cache_.get_qp_info(qp_num, qp_info)) {
        // 缓存命中，直接返回缓存的QP信息
        return qp_num;
    }
    
    // 缓存未命中，创建新的QP
    qps_[qp_num] = std::make_unique<RdmaQueuePair>(qp_num, max_send_wr, max_recv_wr);
    
    // 更新缓存
    qp_info.qp_num = qp_num;
    qp_info.state = 0; // INIT
    qp_info.access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    qp_info.port_num = 1;
    qp_info.pkey_index = 0;
    qp_info.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    cache_.set_qp_info(qp_num, qp_info);
    
    return qp_num;
}

uint32_t RdmaDevice::create_cq(uint32_t max_cqe) {
    uint32_t cq_num = next_cq_num_++;
    
    // 先检查缓存
    RdmaCache::CQInfo cq_info;
    if (cache_.get_cq_info(cq_num, cq_info)) {
        // 缓存命中，直接返回缓存的CQ信息
        return cq_num;
    }
    
    // 缓存未命中，创建新的CQ
    cqs_[cq_num] = std::make_unique<RdmaCompletionQueue>(cq_num, max_cqe);
    
    // 更新缓存
    cq_info.cq_num = cq_num;
    cq_info.cqe = max_cqe;
    cq_info.comp_vector = 0;
    cache_.set_cq_info(cq_num, cq_info);
    
    return cq_num;
}

uint32_t RdmaDevice::register_mr(void* addr, size_t length) {
    uint32_t lkey = next_mr_key_++;
    uint32_t rkey = next_mr_key_++;
    
    // 先检查缓存
    RdmaCache::MRInfo mr_info;
    if (cache_.get_mr_info(lkey, mr_info)) {
        // 缓存命中，直接返回缓存的MR信息
        return lkey;
    }
    
    // 缓存未命中，创建新的MR
    uint32_t access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    mrs_[lkey] = std::make_unique<RdmaMemoryRegion>(addr, length, lkey, rkey, access_flags);
    
    // 更新缓存
    mr_info.lkey = lkey;
    mr_info.rkey = rkey;
    mr_info.addr = reinterpret_cast<uint64_t>(addr);
    mr_info.length = length;
    mr_info.access_flags = access_flags;
    cache_.set_mr_info(lkey, mr_info);
    
    return lkey;
}

RdmaQueuePair* RdmaDevice::get_qp(uint32_t qp_num) {
    // 先检查缓存
    RdmaCache::QPInfo qp_info;
    if (cache_.get_qp_info(qp_num, qp_info)) {
        // 缓存命中，返回QP
        auto it = qps_.find(qp_num);
        if (it != qps_.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

RdmaCompletionQueue* RdmaDevice::get_cq(uint32_t cq_num) {
    // 先检查缓存
    RdmaCache::CQInfo cq_info;
    if (cache_.get_cq_info(cq_num, cq_info)) {
        // 缓存命中，返回CQ
        auto it = cqs_.find(cq_num);
        if (it != cqs_.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

RdmaMemoryRegion* RdmaDevice::get_mr(uint32_t lkey) {
    // 先检查缓存
    RdmaCache::MRInfo mr_info;
    if (cache_.get_mr_info(lkey, mr_info)) {
        // 缓存命中，返回MR
        auto it = mrs_.find(lkey);
        if (it != mrs_.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

void RdmaDevice::start_network_thread() {
    network_thread_ = std::thread([this] { network_processing_loop(); });
}

void RdmaDevice::stop_network_thread() {
    stop_network_ = true;
    if (network_thread_.joinable()) {
        network_thread_.join();
    }
}

void RdmaDevice::network_processing_loop() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1, 100);
    
    while (!stop_network_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        for (auto& qp_pair : qps_) {
            RdmaWorkRequest wr;
            while (qp_pair.second->get_send_request(wr)) {
                bool success = dis(gen) > 5; // 95%成功率
                
                // 检查缓存
                if (wr.op_type == RdmaOpType::READ || wr.op_type == RdmaOpType::WRITE) {
                    // 查找对应的MR
                    for (const auto& mr_pair : mrs_) {
                        if (mr_pair.second->addr() == wr.remote_addr) {
                            // 检查缓存中是否有数据
                            if (cache_.contains(RdmaCache::MR, mr_pair.second->lkey())) {
                                // 缓存命中，可以优化操作
                                success = true; // 缓存命中时总是成功
                            }
                            break;
                        }
                    }
                }
                
                uint32_t cq_num = qp_pair.first;
                if (auto cq = get_cq(cq_num)) {
                    RdmaCompletion comp;
                    comp.wr_id = wr.wr_id;
                    comp.success = success;
                    comp.op_type = wr.op_type;
                    comp.imm_data = wr.imm_data;
                    comp.byte_len = wr.length;
                    comp.src_qp = qp_pair.first;
                    comp.src_qp_num = qp_pair.second->get_qp_num();
                    cq->post_completion(comp);
                }
            }
        }
    }
}

void RdmaDevice::cache_put(void* addr, const void* data, size_t size, uint32_t lkey) {
    // 查找对应的MR
    for (const auto& mr_pair : mrs_) {
        if (mr_pair.second->addr() == addr) {
            // 使用MR的lkey进行缓存存储
            cache_.put(RdmaCache::MR, mr_pair.second->lkey(), data, size);
            break;
        }
    }
}

bool RdmaDevice::cache_get(void* addr, void* buffer, size_t size) {
    // 查找对应的MR
    for (const auto& mr_pair : mrs_) {
        if (mr_pair.second->addr() == addr) {
            // 使用MR的lkey进行缓存查找
            return cache_.get(RdmaCache::MR, mr_pair.second->lkey(), buffer, size);
        }
    }
    return false;
}

RdmaCache::Stats RdmaDevice::get_cache_stats() const {
    return cache_.get_stats();
}

// 批量创建QP
std::vector<uint32_t> RdmaDevice::create_qp_batch(size_t count, uint32_t max_send_wr, uint32_t max_recv_wr) {
    std::vector<uint32_t> qp_nums;
    qp_nums.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        qp_nums.push_back(create_qp(max_send_wr, max_recv_wr));
    }
    
    return qp_nums;
}

// 批量创建CQ
std::vector<uint32_t> RdmaDevice::create_cq_batch(size_t count, uint32_t max_cqe) {
    std::vector<uint32_t> cq_nums;
    cq_nums.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        cq_nums.push_back(create_cq(max_cqe));
    }
    
    return cq_nums;
}

// 批量注册内存区域
std::vector<uint32_t> RdmaDevice::register_mr_batch(const std::vector<std::pair<void*, size_t>>& regions) {
    std::vector<uint32_t> mr_keys;
    mr_keys.reserve(regions.size());
    
    for (const auto& region : regions) {
        mr_keys.push_back(register_mr(region.first, region.second));
    }
    
    return mr_keys;
}

// 获取设备连接数
size_t RdmaDevice::connection_count() const {
    return current_connections_;
}

// 设置最大连接数
void RdmaDevice::set_max_connections(size_t max_conn) {
    max_connections_ = max_conn;
}

// 批量删除QP
void RdmaDevice::destroy_qp_batch(const std::vector<uint32_t>& qp_nums) {
    for (uint32_t qp_num : qp_nums) {
        qps_.erase(qp_num);
    }
}

// 批量删除CQ
void RdmaDevice::destroy_cq_batch(const std::vector<uint32_t>& cq_nums) {
    for (uint32_t cq_num : cq_nums) {
        cqs_.erase(cq_num);
    }
}

// 批量注销MR
void RdmaDevice::deregister_mr_batch(const std::vector<uint32_t>& mr_keys) {
    for (uint32_t mr_key : mr_keys) {
        mrs_.erase(mr_key);
    }
}
