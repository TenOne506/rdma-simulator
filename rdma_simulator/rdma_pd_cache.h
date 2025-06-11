#ifndef RDMA_PD_CACHE_H
#define RDMA_PD_CACHE_H

#include "rdma_cache_base.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <chrono>
#include <vector>
#include <memory>
#include <unordered_set>
#include <queue>
#include <algorithm>

// PD缓存值
struct PDCacheValue {
    uint32_t pd_handle;
    uint32_t context_id;
    std::unordered_map<std::string, std::vector<uint32_t>> resources;
    std::chrono::steady_clock::time_point last_access;
    uint64_t access_count;
};

// PD树节点
struct PDTreeNode {
    uint32_t pd_handle;
    std::vector<std::shared_ptr<PDTreeNode>> children;
    std::weak_ptr<PDTreeNode> parent;
    PDCacheValue value;
};

class RdmaPDCache : public RdmaCacheBase<uint32_t, PDCacheValue> {
public:
    RdmaPDCache(size_t max_size, CachePolicy policy = CachePolicy::LRU)
        : RdmaCacheBase(max_size, policy) {}
    
    bool contains(const uint32_t& pd_handle) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_map_.find(pd_handle) != cache_map_.end();
    }
    
    bool get(const uint32_t& pd_handle, PDCacheValue& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto start_time = std::chrono::high_resolution_clock::now();
        
        auto it = cache_map_.find(pd_handle);
        if (it == cache_map_.end()) {
            metrics_.hit_rate.misses++;
            return false;
        }
        
        value = it->second;
        update_access_order(pd_handle);
        value.last_access = std::chrono::steady_clock::now();
        value.access_count++;
        
        auto end_time = std::chrono::high_resolution_clock::now();
        metrics_.access_time.total_time += (end_time - start_time);
        metrics_.access_time.access_count++;
        metrics_.hit_rate.hits++;
        
        return true;
    }
    
    void put(const uint32_t& pd_handle, const PDCacheValue& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查是否需要驱逐
        if (current_size_ >= max_size_) {
            evict();
        }
        
        cache_map_[pd_handle] = value;
        update_access_order(pd_handle);
        current_size_++;
        
        // 更新树结构
        update_tree_structure(pd_handle, value);
        
        // 更新内存使用统计
        metrics_.memory_usage.original_size += sizeof(PDCacheValue) + 
            get_value_size(value);
        metrics_.memory_usage.compressed_size += sizeof(PDCacheValue);
    }
    
    // 添加资源到PD
    void add_resource(uint32_t pd_handle, uint32_t resource_id, const std::string& resource_type) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(pd_handle);
        if (it != cache_map_.end()) {
            it->second.resources[resource_type].push_back(resource_id);
            update_access_order(pd_handle);
        }
    }
    
    // 从PD移除资源
    void remove_resource(uint32_t pd_handle, uint32_t resource_id, const std::string& resource_type) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(pd_handle);
        if (it != cache_map_.end()) {
            auto& resources = it->second.resources[resource_type];
            resources.erase(
                std::remove(resources.begin(), resources.end(), resource_id),
                resources.end()
            );
            update_access_order(pd_handle);
        }
    }
    
    void remove(const uint32_t& pd_handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(pd_handle);
        if (it != cache_map_.end()) {
            // 从树结构中移除
            remove_from_tree(pd_handle);
            cache_map_.erase(it);
            access_order_.remove(pd_handle);
            current_size_--;
        }
    }
    
    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_map_.clear();
        access_order_.clear();
        current_size_ = 0;
        metrics_ = CacheMetrics{};
        root_node_.reset();
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
    size_t get_value_size(const PDCacheValue& value) const override {
        size_t size = sizeof(value);
        for (const auto& resource_pair : value.resources) {
            size += resource_pair.first.size() + 
                    resource_pair.second.size() * sizeof(uint32_t);
        }
        return size;
    }
    
private:
    std::shared_ptr<PDTreeNode> root_node_;
    
    void update_tree_structure(uint32_t pd_handle, const PDCacheValue& value) {
        if (!root_node_) {
            root_node_ = std::make_shared<PDTreeNode>();
            root_node_->pd_handle = pd_handle;
            root_node_->value = value;
            return;
        }
        
        // 查找或创建节点
        auto node = find_or_create_node(pd_handle);
        if (node) {
            node->value = value;
        }
    }
    
    std::shared_ptr<PDTreeNode> find_or_create_node(uint32_t pd_handle) {
        if (!root_node_) return nullptr;
        
        // 广度优先搜索
        std::queue<std::shared_ptr<PDTreeNode>> queue;
        queue.push(root_node_);
        
        while (!queue.empty()) {
            auto current = queue.front();
            queue.pop();
            
            if (current->pd_handle == pd_handle) {
                return current;
            }
            
            for (const auto& child : current->children) {
                queue.push(child);
            }
        }
        
        // 创建新节点
        auto new_node = std::make_shared<PDTreeNode>();
        new_node->pd_handle = pd_handle;
        root_node_->children.push_back(new_node);
        new_node->parent = root_node_;
        return new_node;
    }
    
    void remove_from_tree(uint32_t pd_handle) {
        if (!root_node_) return;
        
        // 广度优先搜索
        std::queue<std::shared_ptr<PDTreeNode>> queue;
        queue.push(root_node_);
        
        while (!queue.empty()) {
            auto current = queue.front();
            queue.pop();
            
            if (current->pd_handle == pd_handle) {
                if (auto parent = current->parent.lock()) {
                    // 从父节点的子节点列表中移除
                    parent->children.erase(
                        std::remove(parent->children.begin(), parent->children.end(), current),
                        parent->children.end()
                    );
                }
                return;
            }
            
            for (const auto& child : current->children) {
                queue.push(child);
            }
        }
    }
    
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

#endif // RDMA_PD_CACHE_H 