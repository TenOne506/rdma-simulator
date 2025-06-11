#include "rdma_cache.h"
#include <algorithm>
#include <cstring>

// RdmaCache::RdmaCache(size_t max_size, CachePolicy policy)
//     : max_size_(max_size), current_size_(0), policy_(policy),
//       stats_{0, 0, 0} {}

bool RdmaCache::contains(ComponentType type, uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (type) {
        case ComponentType::QP:
            return qp_cache_->contains(id);
        case ComponentType::CQ:
            return cq_cache_->contains(id);
        case ComponentType::MR:
            return mr_cache_->contains(id);
        case ComponentType::PD:
            return pd_cache_->contains(id);
        default:
            return false;
    }
}

bool RdmaCache::get(ComponentType type, uint32_t id, void* buffer, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (type) {
        case ComponentType::QP: {
            QPCacheValue value;
            if (qp_cache_->get(id, value)) {
                std::memcpy(buffer, &value, std::min(size, sizeof(value)));
                return true;
            }
            break;
        }
        case ComponentType::CQ: {
            CQCacheValue value;
            if (cq_cache_->get(id, value)) {
                std::memcpy(buffer, &value, std::min(size, sizeof(value)));
                return true;
            }
            break;
        }
        case ComponentType::MR: {
            MRCacheValue value;
            if (mr_cache_->get(id, value)) {
                std::memcpy(buffer, &value, std::min(size, sizeof(value)));
                return true;
            }
            break;
        }
        case ComponentType::PD: {
            PDCacheValue value;
            if (pd_cache_->get(id, value)) {
                std::memcpy(buffer, &value, std::min(size, sizeof(value)));
                return true;
            }
            break;
        }
    }
    return false;
}

void RdmaCache::put(ComponentType type, uint32_t id, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (type) {
        case ComponentType::QP: {
            QPCacheValue value;
            std::memcpy(&value, data, std::min(size, sizeof(value)));
            qp_cache_->put(id, value);
            break;
        }
        case ComponentType::CQ: {
            CQCacheValue value;
            std::memcpy(&value, data, std::min(size, sizeof(value)));
            cq_cache_->put(id, value);
            break;
        }
        case ComponentType::MR: {
            MRCacheValue value;
            std::memcpy(&value, data, std::min(size, sizeof(value)));
            mr_cache_->put(id, value);
            break;
        }
        case ComponentType::PD: {
            PDCacheValue value;
            std::memcpy(&value, data, std::min(size, sizeof(value)));
            pd_cache_->put(id, value);
            break;
        }
    }
}

// QP缓存操作
bool RdmaCache::get_qp_info(uint32_t qp_num, QPCacheValue& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    return qp_cache_->get(qp_num, info);
}

void RdmaCache::set_qp_info(uint32_t qp_num, const QPCacheValue& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    qp_cache_->put(qp_num, info);
}

// CQ缓存操作
bool RdmaCache::get_cq_info(uint32_t cq_num, CQCacheValue& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    return cq_cache_->get(cq_num, info);
}

void RdmaCache::set_cq_info(uint32_t cq_num, const CQCacheValue& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    cq_cache_->put(cq_num, info);
}

void RdmaCache::batch_add_completions(uint32_t cq_num, const std::vector<CompletionEntry>& completions) {
    std::lock_guard<std::mutex> lock(mutex_);
    CQCacheValue cq_value;
    if (cq_cache_->get(cq_num, cq_value)) {
        cq_value.completions.insert(cq_value.completions.end(), completions.begin(), completions.end());
        cq_cache_->put(cq_num, cq_value);
    }
}

std::vector<CompletionEntry> RdmaCache::batch_get_completions(uint32_t cq_num, uint32_t max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    CQCacheValue cq_value;
    if (cq_cache_->get(cq_num, cq_value)) {
        std::vector<CompletionEntry> result;
        size_t count = std::min(static_cast<size_t>(max_count), cq_value.completions.size());
        result.insert(result.end(), cq_value.completions.begin(), cq_value.completions.begin() + count);
        cq_value.completions.erase(cq_value.completions.begin(), cq_value.completions.begin() + count);
        cq_cache_->put(cq_num, cq_value);
        return result;
    }
    return {};
}

// MR缓存操作
bool RdmaCache::get_mr_info(uint32_t lkey, MRCacheValue& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    return mr_cache_->get(lkey, info);
}

void RdmaCache::set_mr_info(uint32_t lkey, const MRCacheValue& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    mr_cache_->put(lkey, info);
}

MRBlock* RdmaCache::allocate_block(size_t size, uint32_t flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    return mr_cache_->allocate_block(size, flags);
}

void RdmaCache::free_block(MRBlock* block) {
    if (!block) return;
    std::lock_guard<std::mutex> lock(mutex_);
    mr_cache_->free_block(block);
}

// PD缓存操作
bool RdmaCache::get_pd_info(uint32_t pd_handle, PDCacheValue& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    return pd_cache_->get(pd_handle, info);
}

void RdmaCache::set_pd_info(uint32_t pd_handle, const PDCacheValue& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    pd_cache_->put(pd_handle, info);
}

void RdmaCache::add_resource(uint32_t pd_handle, uint32_t resource_id, const std::string& resource_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    pd_cache_->add_resource(pd_handle, resource_id, resource_type);
}

void RdmaCache::remove_resource(uint32_t pd_handle, uint32_t resource_id, const std::string& resource_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    pd_cache_->remove_resource(pd_handle, resource_id, resource_type);
}

// 缓存管理
void RdmaCache::resize(size_t new_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t qp_cache_size = new_size * 0.3;
    size_t cq_cache_size = new_size * 0.2;
    size_t mr_cache_size = new_size * 0.3;
    size_t pd_cache_size = new_size * 0.2;
    
    qp_cache_->resize(qp_cache_size);
    cq_cache_->resize(cq_cache_size);
    mr_cache_->resize(mr_cache_size);
    pd_cache_->resize(pd_cache_size);
}

void RdmaCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    qp_cache_->clear();
    cq_cache_->clear();
    mr_cache_->clear();
    pd_cache_->clear();
}

CacheMetrics RdmaCache::get_metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheMetrics total_metrics;
    
    // 合并所有缓存的指标
    auto qp_metrics = qp_cache_->get_metrics();
    auto cq_metrics = cq_cache_->get_metrics();
    auto mr_metrics = mr_cache_->get_metrics();
    auto pd_metrics = pd_cache_->get_metrics();
    
    // 合并命中率
    total_metrics.hit_rate.hits = qp_metrics.hit_rate.hits + 
                                cq_metrics.hit_rate.hits + 
                                mr_metrics.hit_rate.hits + 
                                pd_metrics.hit_rate.hits;
    total_metrics.hit_rate.misses = qp_metrics.hit_rate.misses + 
                                  cq_metrics.hit_rate.misses + 
                                  mr_metrics.hit_rate.misses + 
                                  pd_metrics.hit_rate.misses;
    
    // 合并访问时间
    total_metrics.access_time.total_time = qp_metrics.access_time.total_time +
                                         cq_metrics.access_time.total_time +
                                         mr_metrics.access_time.total_time +
                                         pd_metrics.access_time.total_time;
    total_metrics.access_time.access_count = qp_metrics.access_time.access_count +
                                           cq_metrics.access_time.access_count +
                                           mr_metrics.access_time.access_count +
                                           pd_metrics.access_time.access_count;
    
    // 合并内存使用
    total_metrics.memory_usage.original_size = qp_metrics.memory_usage.original_size +
                                             cq_metrics.memory_usage.original_size +
                                             mr_metrics.memory_usage.original_size +
                                             pd_metrics.memory_usage.original_size;
    total_metrics.memory_usage.compressed_size = qp_metrics.memory_usage.compressed_size +
                                               cq_metrics.memory_usage.compressed_size +
                                               mr_metrics.memory_usage.compressed_size +
                                               pd_metrics.memory_usage.compressed_size;
    
    return total_metrics;
}

Stats RdmaCache::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats total_stats{0, 0, 0};
    
    // 合并所有缓存的统计信息
    auto qp_metrics = qp_cache_->get_metrics();
    auto cq_metrics = cq_cache_->get_metrics();
    auto mr_metrics = mr_cache_->get_metrics();
    auto pd_metrics = pd_cache_->get_metrics();
    
    total_stats.hits = qp_metrics.hit_rate.hits + 
                      cq_metrics.hit_rate.hits + 
                      mr_metrics.hit_rate.hits + 
                      pd_metrics.hit_rate.hits;
    total_stats.misses = qp_metrics.hit_rate.misses + 
                        cq_metrics.hit_rate.misses + 
                        mr_metrics.hit_rate.misses + 
                        pd_metrics.hit_rate.misses;
    total_stats.evictions = qp_metrics.memory_usage.original_size - qp_metrics.memory_usage.compressed_size +
                           cq_metrics.memory_usage.original_size - cq_metrics.memory_usage.compressed_size +
                           mr_metrics.memory_usage.original_size - mr_metrics.memory_usage.compressed_size +
                           pd_metrics.memory_usage.original_size - pd_metrics.memory_usage.compressed_size;
    
    return total_stats;
}

void RdmaCache::evict(ComponentType type) {
    switch (type) {
        case ComponentType::QP:
            qp_cache_->clear();
            break;
        case ComponentType::CQ:
            cq_cache_->clear();
            break;
        case ComponentType::MR:
            mr_cache_->clear();
            break;
        case ComponentType::PD:
            pd_cache_->clear();
            break;
    }
}

void RdmaCache::update_access_order(ComponentType type, uint32_t id) {
    // 由于各个缓存组件都有自己的访问顺序管理，这里不需要额外操作
    (void)type;
    (void)id;
}