#ifndef RDMA_QP_CACHE_H
#define RDMA_QP_CACHE_H

#include "rdma_cache_base.h"
#include <cstdint>
#include <chrono>

// 压缩的QP状态信息
struct CompressedQPState {
    uint32_t state : 4;          // QP状态
    uint32_t access_flags : 8;   // 访问标志
    uint32_t port_num : 4;       // 端口号
    uint32_t pkey_index : 8;     // P_Key索引
    uint32_t qp_access_flags : 8; // QP访问标志
};

// QP缓存值
struct QPCacheValue {
    CompressedQPState state;
    uint32_t qp_num;
    uint32_t pd_handle;
    uint32_t send_cq;
    uint32_t recv_cq;
    std::chrono::steady_clock::time_point last_access;
    uint64_t access_count;
};

class RdmaQPCache : public RdmaCacheBase<uint32_t, QPCacheValue> {
public:
    RdmaQPCache(size_t max_size, CachePolicy policy = CachePolicy::LRU)
        : RdmaCacheBase(max_size, policy) {}

protected:
    size_t get_value_size(const QPCacheValue& value) const override {
        return sizeof(value);
    }
};

#endif // RDMA_QP_CACHE_H 