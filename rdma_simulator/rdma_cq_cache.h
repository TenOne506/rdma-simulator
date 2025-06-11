#ifndef RDMA_CQ_CACHE_H
#define RDMA_CQ_CACHE_H

#include "rdma_cache_base.h"
#include <cstdint>
#include <vector>
#include <chrono>

// 完成队列条目
struct CompletionEntry {
    uint32_t wr_id;
    uint32_t status;
    uint32_t opcode;
    uint32_t byte_len;
    uint64_t imm_data;
};

// CQ缓存值
struct CQCacheValue {
    std::vector<CompletionEntry> completions;
    uint32_t cq_num;
    uint32_t pd_handle;
    std::chrono::steady_clock::time_point last_access;
    uint64_t access_count;
};

class RdmaCQCache : public RdmaCacheBase<uint32_t, CQCacheValue> {
public:
    RdmaCQCache(size_t max_size, CachePolicy policy = CachePolicy::LRU)
        : RdmaCacheBase(max_size, policy) {}
    
    bool contains(const uint32_t& cq_num) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_map_.find(cq_num) != cache_map_.end();
    }
    
    bool get(const uint32_t& cq_num, CQCacheValue& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto start_time = std::chrono::high_resolution_clock::now();
        
        auto it = cache_map_.find(cq_num);
        if (it == cache_map_.end()) {
            metrics_.hit_rate.misses++;
            return false;
        }
        
        value = it->second;
        update_access_order(cq_num);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        metrics_.access_time.total_time += (end_time - start_time);
        metrics_.access_time.access_count++;
        metrics_.hit_rate.hits++;
        
        return true;
    }
    
    void put(const uint32_t& cq_num, const CQCacheValue& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查是否需要驱逐
        if (current_size_ >= max_size_) {
            evict();
        }
        
        cache_map_[cq_num] = value;
        update_access_order(cq_num);
        current_size_++;
        
        // 更新内存使用统计
        metrics_.memory_usage.original_size += sizeof(CQCacheValue) + 
            value.completions.size() * sizeof(CompletionEntry);
        metrics_.memory_usage.compressed_size += sizeof(CQCacheValue) + 
            value.access_count * sizeof(CompletionEntry);
    }
    
    // 批量添加完成事件
    void batch_add_completions(uint32_t cq_num, const std::vector<CompletionEntry>& new_completions) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(cq_num);
        if (it != cache_map_.end()) {
            it->second.completions.insert(it->second.completions.end(),
                                        new_completions.begin(),
                                        new_completions.end());
            update_access_order(cq_num);
        }
    }
    
    // 批量获取完成事件
    std::vector<CompletionEntry> batch_get_completions(uint32_t cq_num, uint32_t max_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(cq_num);
        if (it == cache_map_.end()) {
            return {};
        }

        std::vector<CompletionEntry> result;
        auto& completions = it->second.completions;
        size_t count = std::min(static_cast<size_t>(max_count), completions.size());
        
        result.insert(result.end(), completions.begin(), completions.begin() + count);
        completions.erase(completions.begin(), completions.begin() + count);
        
        update_access_order(cq_num);
        return result;
    }
    
    void remove(const uint32_t& cq_num) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(cq_num);
        if (it != cache_map_.end()) {
            cache_map_.erase(it);
            access_order_.remove(cq_num);
            current_size_--;
        }
    }
    
    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
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
    size_t get_value_size(const CQCacheValue& value) const override {
        return sizeof(value) + value.completions.size() * sizeof(CompletionEntry);
    }
    
private:
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

#endif // RDMA_CQ_CACHE_H 