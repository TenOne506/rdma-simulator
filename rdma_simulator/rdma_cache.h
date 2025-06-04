#ifndef RDMA_CACHE_H
#define RDMA_CACHE_H

#include <list>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <vector>
#include "rdma_memory_region.h"

class RdmaCache {
public:
    enum CachePolicy {
        LRU,    // 最近最少使用
        MRU,    // 最近使用
        FIFO    // 先进先出
    };
    
    RdmaCache(size_t max_size, CachePolicy policy = LRU);
    
    // 检查缓存中是否存在指定地址
    bool contains(void* addr) const;
    
    // 获取缓存中的数据
    bool get(void* addr, void* buffer, size_t size);
    
    // 将数据放入缓存
    void put(void* addr, const void* data, size_t size, uint32_t lkey);
    
    // 缓存统计信息
    struct Stats {
        uint64_t hits;
        uint64_t misses;
        uint64_t evictions;
    };
    
    Stats get_stats() const;
    
    // 清除缓存
    void clear();
    
    // 调整缓存大小
    void resize(size_t new_size);

private:
    struct CacheEntry {
        void* addr;
        std::vector<uint8_t> data;
        uint32_t lkey;
        size_t size;
    };
    
    void evict();
    void update_access_order(void* addr);
    
    size_t max_size_;
    size_t current_size_;
    CachePolicy policy_;
    mutable std::mutex mutex_;
    
    // 用于快速查找
    std::unordered_map<void*, CacheEntry> cache_map_;
    
    // 用于维护访问顺序 (LRU/MRU/FIFO)
    std::list<void*> access_order_;
    
    // 统计信息
    Stats stats_;
};

#endif // RDMA_CACHE_H