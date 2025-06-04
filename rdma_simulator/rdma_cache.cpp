#include "rdma_cache.h"
#include <algorithm>
#include <cstring>

RdmaCache::RdmaCache(size_t max_size, CachePolicy policy)
    : max_size_(max_size), current_size_(0), policy_(policy),
      stats_{0, 0, 0} {}

bool RdmaCache::contains(void* addr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_map_.find(addr) != cache_map_.end();
}

bool RdmaCache::get(void* addr, void* buffer, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(addr);
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
    update_access_order(addr);
    stats_.hits++;
    return true;
}

void RdmaCache::put(void* addr, const void* data, size_t size, uint32_t lkey) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查是否已存在
    if (cache_map_.find(addr) != cache_map_.end()) {
        // 更新现有条目
        CacheEntry& entry = cache_map_[addr];
        current_size_ -= entry.size;
        entry.data.resize(size);
        std::memcpy(entry.data.data(), data, size);
        entry.size = size;
        entry.lkey = lkey;
        current_size_ += size;
        update_access_order(addr);
        return;
    }
    
    // 检查是否需要驱逐
    while (current_size_ + size > max_size_) {
        evict();
    }
    
    // 添加新条目
    CacheEntry entry;
    entry.addr = addr;
    entry.data.resize(size);
    std::memcpy(entry.data.data(), data, size);
    entry.size = size;
    entry.lkey = lkey;
    
    cache_map_[addr] = entry;
    current_size_ += size;
    access_order_.push_back(addr);
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
    
    void* addr_to_evict = nullptr;
    
    switch (policy_) {
        case LRU:
            addr_to_evict = access_order_.front();
            access_order_.pop_front();
            break;
        case MRU:
            addr_to_evict = access_order_.back();
            access_order_.pop_back();
            break;
        case FIFO:
            addr_to_evict = access_order_.front();
            access_order_.pop_front();
            break;
    }
    
    auto it = cache_map_.find(addr_to_evict);
    if (it != cache_map_.end()) {
        current_size_ -= it->second.size;
        cache_map_.erase(it);
        stats_.evictions++;
    }
}

void RdmaCache::update_access_order(void* addr) {
    // 对于FIFO策略，不更新访问顺序
    if (policy_ == FIFO) return;
    
    // 找到并移除现有条目
    auto it = std::find(access_order_.begin(), access_order_.end(), addr);
    if (it != access_order_.end()) {
        access_order_.erase(it);
    }
    
    // 根据策略添加到适当位置
    if (policy_ == LRU) {
        access_order_.push_back(addr);  // LRU: 最近访问的放到最后
    } else if (policy_ == MRU) {
        access_order_.push_front(addr);  // MRU: 最近访问的放到前面
    }
}