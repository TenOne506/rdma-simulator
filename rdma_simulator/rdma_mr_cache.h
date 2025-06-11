#ifndef RDMA_MR_CACHE_H
#define RDMA_MR_CACHE_H

#include "rdma_cache_base.h"
#include <cstdint>
#include <vector>
#include <chrono>

// 内存块结构
struct MRBlock {
    void* addr;
    size_t size;
    uint32_t flags;
    bool in_use;
    std::chrono::steady_clock::time_point last_access;
    uint64_t access_count;
};

// MR缓存值
struct MRCacheValue {
    uint32_t lkey;
    uint32_t rkey;
    uint64_t addr;
    uint64_t length;
    uint32_t access_flags;
    uint32_t pd_handle;
    std::vector<MRBlock> blocks;
};

class RdmaMRCache : public RdmaCacheBase<uint32_t, MRCacheValue> {
public:
    RdmaMRCache(size_t max_size, CachePolicy policy = CachePolicy::LRU)
        : RdmaCacheBase(max_size, policy) {}
    
    bool contains(const uint32_t& lkey) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_map_.find(lkey) != cache_map_.end();
    }
    
    bool get(const uint32_t& lkey, MRCacheValue& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto start_time = std::chrono::high_resolution_clock::now();
        
        auto it = cache_map_.find(lkey);
        if (it == cache_map_.end()) {
            metrics_.hit_rate.misses++;
            return false;
        }
        
        value = it->second;
        update_access_order(lkey);
        
        // 更新访问统计
        for (auto& block : value.blocks) {
            if (block.in_use) {
                block.last_access = std::chrono::steady_clock::now();
                block.access_count++;
            }
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        metrics_.access_time.total_time += (end_time - start_time);
        metrics_.access_time.access_count++;
        metrics_.hit_rate.hits++;
        
        return true;
    }
    
    void put(const uint32_t& lkey, const MRCacheValue& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查是否需要驱逐
        if (current_size_ >= max_size_) {
            evict();
        }
        
        cache_map_[lkey] = value;
        update_access_order(lkey);
        current_size_++;
        
        // 更新内存使用统计
        size_t total_size = sizeof(MRCacheValue);
        for (const auto& block : value.blocks) {
            total_size += block.size;
        }
        metrics_.memory_usage.original_size += total_size;
        metrics_.memory_usage.compressed_size += sizeof(MRCacheValue);
    }
    
    // 分配内存块
    MRBlock* allocate_block(size_t size, uint32_t flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& block : free_blocks_) {
            if (block.size >= size && !block.in_use) {
                block.in_use = true;
                block.flags = flags;
                block.last_access = std::chrono::steady_clock::now();
                block.access_count++;
                return &block;
            }
        }
        return nullptr;
    }
    
    // 释放内存块
    void free_block(MRBlock* block) {
        if (!block) return;
        std::lock_guard<std::mutex> lock(mutex_);
        block->in_use = false;
    }
    
    void remove(const uint32_t& lkey) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(lkey);
        if (it != cache_map_.end()) {
            // 释放所有内存块
            for (auto& block : it->second.blocks) {
                if (block.in_use) {
                    free_block(&block);
                }
            }
            cache_map_.erase(it);
            access_order_.remove(lkey);
            current_size_--;
        }
    }
    
    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& mr : cache_map_) {
            for (auto& block : mr.second.blocks) {
                if (block.in_use) {
                    free_block(&block);
                }
            }
        }
        cache_map_.clear();
        access_order_.clear();
        current_size_ = 0;
        metrics_ = CacheMetrics{};
    }
    
    void resize(size_t new_size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        max_size_ = new_size;
        while (current_size_ > max_size_) {
            evict();
        }
    }
    
    CacheMetrics get_metrics() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_;
    }
    
protected:
    size_t get_value_size(const MRCacheValue& value) const override {
        return sizeof(value) + value.blocks.size() * sizeof(MRBlock);
    }

private:
    std::vector<MRBlock> free_blocks_;
    
    void evict() {
        if (access_order_.empty()) return;
        
        uint32_t key_to_evict;
        if (policy_ == CachePolicy::LRU || policy_ == CachePolicy::FIFO) {
            key_to_evict = access_order_.front();
            access_order_.pop_front();
        } else { // MRU
            key_to_evict = access_order_.back();
            access_order_.pop_back();
        }
        
        remove(key_to_evict);
    }
};

#endif // RDMA_MR_CACHE_H 