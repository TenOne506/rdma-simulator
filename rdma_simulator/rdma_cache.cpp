#include "rdma_cache.h"
#include <algorithm>
#include <cstring>

RdmaCache::RdmaCache(size_t max_size, CachePolicy policy)
    : max_size_(max_size), current_size_(0), policy_(policy),
      stats_{0, 0, 0} {}

bool RdmaCache::contains(ComponentType type, uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_map_.find(make_key(type, id)) != cache_map_.end();
}

bool RdmaCache::get(ComponentType type, uint32_t id, void* buffer, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(make_key(type, id));
    if (it == cache_map_.end()) {
        stats_.misses++;
        return false;
    }
    
    const CacheEntry& entry = it->second;
    if (size > entry.size) {
        stats_.misses++;
        return false;
    }
    
    std::memcpy(buffer, entry.data.data(), size);
    update_access_order(type, id);
    stats_.hits++;
    return true;
}

void RdmaCache::put(ComponentType type, uint32_t id, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint64_t key = make_key(type, id);
    
    // 检查是否已存在
    if (cache_map_.find(key) != cache_map_.end()) {
        // 更新现有条目
        CacheEntry& entry = cache_map_[key];
        current_size_ -= entry.size;
        entry.data.resize(size);
        std::memcpy(entry.data.data(), data, size);
        entry.size = size;
        current_size_ += size;
        update_access_order(type, id);
        return;
    }
    
    // 检查是否需要驱逐
    while (current_size_ + size > max_size_) {
        evict();
    }
    
    // 添加新条目
    CacheEntry entry;
    entry.type = type;
    entry.id = id;
    entry.data.resize(size);
    std::memcpy(entry.data.data(), data, size);
    entry.size = size;
    
    cache_map_[key] = entry;
    current_size_ += size;
    access_order_.push_back(key);
}

bool RdmaCache::get_qp_info(uint32_t qp_num, QPInfo& info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(make_key(QP, qp_num));
    if (it == cache_map_.end()) {
        return false;
    }
    info = it->second.component_info.qp_info;
    return true;
}

bool RdmaCache::get_cq_info(uint32_t cq_num, CQInfo& info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(make_key(CQ, cq_num));
    if (it == cache_map_.end()) {
        return false;
    }
    info = it->second.component_info.cq_info;
    return true;
}

bool RdmaCache::get_mr_info(uint32_t lkey, MRInfo& info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(make_key(MR, lkey));
    if (it == cache_map_.end()) {
        return false;
    }
    info = it->second.component_info.mr_info;
    return true;
}

bool RdmaCache::get_pd_info(uint32_t pd_handle, PDInfo& info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(make_key(PD, pd_handle));
    if (it == cache_map_.end()) {
        return false;
    }
    info = it->second.component_info.pd_info;
    return true;
}

void RdmaCache::set_qp_info(uint32_t qp_num, const QPInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = make_key(QP, qp_num);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        it->second.component_info.qp_info = info;
    }
}

void RdmaCache::set_cq_info(uint32_t cq_num, const CQInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = make_key(CQ, cq_num);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        it->second.component_info.cq_info = info;
    }
}

void RdmaCache::set_mr_info(uint32_t lkey, const MRInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = make_key(MR, lkey);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        it->second.component_info.mr_info = info;
    }
}

void RdmaCache::set_pd_info(uint32_t pd_handle, const PDInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = make_key(PD, pd_handle);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        it->second.component_info.pd_info = info;
    }
}

RdmaCache::Stats RdmaCache::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void RdmaCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_map_.clear();
    access_order_.clear();
    current_size_ = 0;
    stats_ = {0, 0, 0};
}

void RdmaCache::resize(size_t new_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_size_ = new_size;
    while (current_size_ > max_size_) {
        evict();
    }
}

void RdmaCache::evict() {
    if (access_order_.empty()) return;
    
    uint64_t key_to_evict = 0;
    
    switch (policy_) {
        case LRU:
            key_to_evict = access_order_.front();
            access_order_.pop_front();
            break;
        case MRU:
            key_to_evict = access_order_.back();
            access_order_.pop_back();
            break;
        case FIFO:
            key_to_evict = access_order_.front();
            access_order_.pop_front();
            break;
    }
    
    auto it = cache_map_.find(key_to_evict);
    if (it != cache_map_.end()) {
        current_size_ -= it->second.size;
        cache_map_.erase(it);
        stats_.evictions++;
    }
}

void RdmaCache::update_access_order(ComponentType type, uint32_t id) {
    // 对于FIFO策略，不更新访问顺序
    if (policy_ == FIFO) return;
    
    uint64_t key = make_key(type, id);
    
    // 找到并移除现有条目
    auto it = std::find(access_order_.begin(), access_order_.end(), key);
    if (it != access_order_.end()) {
        access_order_.erase(it);
    }
    
    // 根据策略添加到适当位置
    if (policy_ == LRU) {
        access_order_.push_back(key);  // LRU: 最近访问的放到最后
    } else if (policy_ == MRU) {
        access_order_.push_front(key);  // MRU: 最近访问的放到前面
    }
}