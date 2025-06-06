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

    // RDMA设备组件类型
    enum ComponentType {
        PD,     // Protection Domain
        QP,     // Queue Pair
        CQ,     // Completion Queue
        MR      // Memory Region
    };
    
    RdmaCache(size_t max_size, CachePolicy policy = LRU);
    
    // 检查缓存中是否存在指定组件
    bool contains(ComponentType type, uint32_t id) const;
    
    // 获取缓存中的组件信息
    bool get(ComponentType type, uint32_t id, void* buffer, size_t size);
    
    // 将组件信息放入缓存
    void put(ComponentType type, uint32_t id, const void* data, size_t size);
    
    // 获取PD信息
    struct PDInfo {
        uint32_t pd_handle;
        uint32_t context_id;
        std::vector<uint32_t> mr_list;    // 属于该PD的MR列表
        std::vector<uint32_t> qp_list;    // 属于该PD的QP列表

        PDInfo() : pd_handle(0), context_id(0) {}
    };
    
    // 获取QP信息
    struct QPInfo {
        uint32_t qp_num;
        uint32_t state;
        uint32_t access_flags;
        uint32_t port_num;
        uint32_t pkey_index;
        uint32_t qp_access_flags;
        uint32_t pd_handle;      // 所属的PD句柄

        QPInfo() : qp_num(0), state(0), access_flags(0), port_num(0), 
                  pkey_index(0), qp_access_flags(0), pd_handle(0) {}
    };
    
    // 获取CQ信息
    struct CQInfo {
        uint32_t cq_num;
        uint32_t cqe;
        uint32_t comp_vector;
        uint32_t pd_handle;      // 所属的PD句柄

        CQInfo() : cq_num(0), cqe(0), comp_vector(0), pd_handle(0) {}
    };
    
    // 获取MR信息
    struct MRInfo {
        uint32_t lkey;
        uint32_t rkey;
        uint64_t addr;
        uint64_t length;
        uint32_t access_flags;
        uint32_t pd_handle;      // 所属的PD句柄

        MRInfo() : lkey(0), rkey(0), addr(0), length(0), access_flags(0), pd_handle(0) {}
    };
    
    // 获取组件信息
    bool get_pd_info(uint32_t pd_handle, PDInfo& info) const;
    bool get_qp_info(uint32_t qp_num, QPInfo& info) const;
    bool get_cq_info(uint32_t cq_num, CQInfo& info) const;
    bool get_mr_info(uint32_t lkey, MRInfo& info) const;
    
    // 设置组件信息
    void set_pd_info(uint32_t pd_handle, const PDInfo& info);
    void set_qp_info(uint32_t qp_num, const QPInfo& info);
    void set_cq_info(uint32_t cq_num, const CQInfo& info);
    void set_mr_info(uint32_t lkey, const MRInfo& info);
    
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
        ComponentType type;
        uint32_t id;
        std::vector<uint8_t> data;
        size_t size;
        
        // 组件特定信息
        struct ComponentInfo {
            union {
                PDInfo pd_info;
                QPInfo qp_info;
                CQInfo cq_info;
                MRInfo mr_info;
            };
            ComponentType type;

            ComponentInfo() : type(PD) {
                ::new (&pd_info) PDInfo();
            }

            ComponentInfo(const ComponentInfo& other) : type(other.type) {
                switch (type) {
                    case PD:
                        ::new (&pd_info) PDInfo(other.pd_info);
                        break;
                    case QP:
                        ::new (&qp_info) QPInfo(other.qp_info);
                        break;
                    case CQ:
                        ::new (&cq_info) CQInfo(other.cq_info);
                        break;
                    case MR:
                        ::new (&mr_info) MRInfo(other.mr_info);
                        break;
                }
            }

            ComponentInfo(ComponentInfo&& other) noexcept : type(other.type) {
                switch (type) {
                    case PD:
                        ::new (&pd_info) PDInfo(std::move(other.pd_info));
                        break;
                    case QP:
                        ::new (&qp_info) QPInfo(std::move(other.qp_info));
                        break;
                    case CQ:
                        ::new (&cq_info) CQInfo(std::move(other.cq_info));
                        break;
                    case MR:
                        ::new (&mr_info) MRInfo(std::move(other.mr_info));
                        break;
                }
            }

            ComponentInfo& operator=(const ComponentInfo& other) {
                if (this != &other) {
                    // 清理当前对象
                    switch (type) {
                        case PD:
                            pd_info.~PDInfo();
                            break;
                        case QP:
                            qp_info.~QPInfo();
                            break;
                        case CQ:
                            cq_info.~CQInfo();
                            break;
                        case MR:
                            mr_info.~MRInfo();
                            break;
                    }

                    // 复制新数据
                    type = other.type;
                    switch (type) {
                        case PD:
                            ::new (&pd_info) PDInfo(other.pd_info);
                            break;
                        case QP:
                            ::new (&qp_info) QPInfo(other.qp_info);
                            break;
                        case CQ:
                            ::new (&cq_info) CQInfo(other.cq_info);
                            break;
                        case MR:
                            ::new (&mr_info) MRInfo(other.mr_info);
                            break;
                    }
                }
                return *this;
            }

            ComponentInfo& operator=(ComponentInfo&& other) noexcept {
                if (this != &other) {
                    // 清理当前对象
                    switch (type) {
                        case PD:
                            pd_info.~PDInfo();
                            break;
                        case QP:
                            qp_info.~QPInfo();
                            break;
                        case CQ:
                            cq_info.~CQInfo();
                            break;
                        case MR:
                            mr_info.~MRInfo();
                            break;
                    }

                    // 移动新数据
                    type = other.type;
                    switch (type) {
                        case PD:
                            ::new (&pd_info) PDInfo(std::move(other.pd_info));
                            break;
                        case QP:
                            ::new (&qp_info) QPInfo(std::move(other.qp_info));
                            break;
                        case CQ:
                            ::new (&cq_info) CQInfo(std::move(other.cq_info));
                            break;
                        case MR:
                            ::new (&mr_info) MRInfo(std::move(other.mr_info));
                            break;
                    }
                }
                return *this;
            }

            ~ComponentInfo() {
                switch (type) {
                    case PD:
                        pd_info.~PDInfo();
                        break;
                    case QP:
                        qp_info.~QPInfo();
                        break;
                    case CQ:
                        cq_info.~CQInfo();
                        break;
                    case MR:
                        mr_info.~MRInfo();
                        break;
                }
            }
        } component_info;

        // 默认构造函数
        CacheEntry() : type(PD), id(0), size(0) {}

        // 复制构造函数
        CacheEntry(const CacheEntry& other) : type(other.type), id(other.id), data(other.data), size(other.size), component_info(other.component_info) {}

        // 移动构造函数
        CacheEntry(CacheEntry&& other) noexcept : type(other.type), id(other.id), data(std::move(other.data)), size(other.size), component_info(std::move(other.component_info)) {}

        // 复制赋值运算符
        CacheEntry& operator=(const CacheEntry& other) {
            if (this != &other) {
                type = other.type;
                id = other.id;
                data = other.data;
                size = other.size;
                component_info = other.component_info;
            }
            return *this;
        }

        // 移动赋值运算符
        CacheEntry& operator=(CacheEntry&& other) noexcept {
            if (this != &other) {
                type = other.type;
                id = other.id;
                data = std::move(other.data);
                size = other.size;
                component_info = std::move(other.component_info);
            }
            return *this;
        }
    };
    
    void evict();
    void update_access_order(ComponentType type, uint32_t id);
    
    size_t max_size_;
    size_t current_size_;
    CachePolicy policy_;
    mutable std::mutex mutex_;
    
    // 用于快速查找
    std::unordered_map<uint64_t, CacheEntry> cache_map_;  // key = (type << 32) | id
    
    // 用于维护访问顺序 (LRU/MRU/FIFO)
    std::list<uint64_t> access_order_;
    
    // 统计信息
    Stats stats_;
    
    // 辅助函数：生成缓存键
    static uint64_t make_key(ComponentType type, uint32_t id) {
        return (static_cast<uint64_t>(type) << 32) | id;
    }
};

#endif // RDMA_CACHE_H