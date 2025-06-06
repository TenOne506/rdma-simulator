#include "rdma_device.h"
#include "rdma_cache.h"
#include <iostream>
#include <random>
#include <chrono>

RdmaDevice::RdmaDevice() 
    : next_qp_num_(1), next_cq_num_(1), next_mr_key_(1), 
    stop_network_(false),
    cache_(1024 * 1024 * 64) {}

RdmaDevice::~RdmaDevice() {
    stop_network_thread();
}

uint32_t RdmaDevice::create_qp(uint32_t max_send_wr, uint32_t max_recv_wr) {
    uint32_t qp_num = next_qp_num_++;
    qps_[qp_num] = std::make_unique<RdmaQueuePair>(qp_num, max_send_wr, max_recv_wr);
    return qp_num;
}

uint32_t RdmaDevice::create_cq(uint32_t max_cqe) {
    uint32_t cq_num = next_cq_num_++;
    cqs_[cq_num] = std::make_unique<RdmaCompletionQueue>(cq_num, max_cqe);
    return cq_num;
}

uint32_t RdmaDevice::register_mr(void* addr, size_t length) {
    uint32_t lkey = next_mr_key_++;
    uint32_t rkey = next_mr_key_++;
    mrs_[lkey] = std::make_unique<RdmaMemoryRegion>(addr, length, lkey, rkey);
    return lkey;
}

RdmaQueuePair* RdmaDevice::get_qp(uint32_t qp_num) {
    auto it = qps_.find(qp_num);
    return it != qps_.end() ? it->second.get() : nullptr;
}

RdmaCompletionQueue* RdmaDevice::get_cq(uint32_t cq_num) {
    auto it = cqs_.find(cq_num);
    return it != cqs_.end() ? it->second.get() : nullptr;
}

RdmaMemoryRegion* RdmaDevice::get_mr(uint32_t lkey) {
    auto it = mrs_.find(lkey);
    return it != mrs_.end() ? it->second.get() : nullptr;
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
            RdmaQueuePair::WorkRequest wr;
            while (qp_pair.second->get_send_request(wr)) {
                bool success = dis(gen) > 5; // 95%成功率
                
                // 检查缓存
                if (wr.op_type == RdmaOpType::READ || wr.op_type == RdmaOpType::WRITE) {
                    if (cache_.contains(RdmaCache::MR, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wr.remote_addr)))) {
                        // 缓存命中，可以优化操作
                    }
                }
                
                uint32_t cq_num = qp_pair.first;
                if (auto cq = get_cq(cq_num)) {
                    cq->post_completion({wr.wr_id, success, wr.op_type});
                }
            }
        }
    }
}

// 添加新的缓存相关方法
void RdmaDevice::cache_put(void* addr, const void* data, size_t size, uint32_t lkey) {
    cache_.put(RdmaCache::MR, lkey, data, size);
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
    
    //std::lock_guard<std::mutex> lock(device_mutex_);
    
    for (size_t i = 0; i < count; ++i) {
        if (current_connections_ > max_connections_) {
            std::cerr << "Warning: Reached max connections (" << max_connections_ 
                      << "), cannot create more QPs" << std::endl;
            break;
        }
        
        uint32_t qp_num = next_qp_num_++;
        auto qp = std::make_unique<RdmaQueuePair>(qp_num, max_send_wr, max_recv_wr);
        qps_[qp_num] = std::move(qp);
        current_connections_++;
        qp_nums.push_back(qp_num);
    }
    
    return qp_nums;
}

std::vector<uint32_t> RdmaDevice::create_cq_batch(size_t count, uint32_t max_cqe) {
    std::vector<uint32_t> cq_nums;
    cq_nums.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        // 检查是否达到最大连接限制
        if (current_connections_ > max_connections_) {
            std::cerr << "Warning: Reached maximum connections limit (" 
                      << max_connections_ << "), cannot create more CQs." << std::endl;
            break;
        }
        
        // 创建单个CQ
        uint32_t cq_num = create_cq(max_cqe);
        if (cq_num == 0) { // 假设0是无效的CQ号
            std::cerr << "Error: Failed to create CQ " << i << " in batch" << std::endl;
            continue;
        }
        
        cq_nums.push_back(cq_num);
        //current_connections_++;
    }
    
    return cq_nums;
}

std::vector<uint32_t> RdmaDevice::register_mr_batch(
    const std::vector<std::pair<void*, size_t>>& regions) {
    
    std::vector<uint32_t> mr_keys;
    mr_keys.reserve(regions.size());
    
    for (const auto& region : regions) {
        // 检查是否达到最大连接限制
        if (current_connections_ > max_connections_) {
            std::cerr << "Warning: Reached maximum connections limit (" 
                      << max_connections_ << "), cannot register more MRs." << std::endl;
            break;
        }
        
        // 检查输入参数有效性
        if (region.first == nullptr || region.second == 0) {
            std::cerr << "Error: Invalid memory region (addr=" << region.first 
                      << ", size=" << region.second << "), skipping." << std::endl;
            mr_keys.push_back(0); // 使用0表示无效的MR key
            continue;
        }
        
        // 注册单个内存区域
        uint32_t mr_key = register_mr(region.first, region.second);
        if (mr_key == 0) { // 假设0是无效的MR key
            std::cerr << "Error: Failed to register MR for address " 
                      << region.first << std::endl;
        }
        
        mr_keys.push_back(mr_key);
        //current_connections_++;
    }
    
    return mr_keys;
}

size_t RdmaDevice::connection_count() const {
    return current_connections_.load();
}

void RdmaDevice::set_max_connections(size_t max_conn) {
    if (max_conn < current_connections_) {
        std::cerr << "Warning: New max connections (" << max_conn 
                  << ") is less than current connections (" 
                  << current_connections_ << ")" << std::endl;
    }
    max_connections_ = max_conn;
}


void RdmaDevice::destroy_qp_batch(const std::vector<uint32_t>& qp_nums) {
    for (auto qp_num : qp_nums) {
        if (qp_num == 0) continue;
        
        if (qps_.erase(qp_num)) {
            current_connections_--;
        }
    }
}

void RdmaDevice::destroy_cq_batch(const std::vector<uint32_t>& cq_nums) {
    for (auto cq_num : cq_nums) {
        if (cq_num == 0) continue;
        
        if (cqs_.erase(cq_num)) {
            current_connections_--;
        }
    }
}

void RdmaDevice::deregister_mr_batch(const std::vector<uint32_t>& mr_keys) {
    for (auto mr_key : mr_keys) {
        if (mr_key == 0) continue;
        
        if (mrs_.erase(mr_key)) {
            current_connections_--;
        }
    }
}
