#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <memory>
#include <vector>

// RDMA access flags
#define IBV_ACCESS_REMOTE_READ    0x1
#define IBV_ACCESS_REMOTE_WRITE   0x2
#define IBV_ACCESS_LOCAL_WRITE    0x4
#define IBV_ACCESS_REMOTE_ATOMIC  0x8

class RdmaMemoryRegion {
public:
    struct Stats {
        uint64_t read_ops{0};
        uint64_t write_ops{0};
        uint64_t atomic_ops{0};
        uint64_t read_bytes{0};
        uint64_t write_bytes{0};
        uint64_t cache_hits{0};
        uint64_t cache_misses{0};
    };

    RdmaMemoryRegion(void* addr, size_t length, uint32_t lkey, uint32_t rkey, uint32_t access_flags);
    ~RdmaMemoryRegion();

    // 基本属性访问
    void* addr() const { return addr_; }
    size_t length() const { return length_; }
    uint32_t lkey() const { return lkey_; }
    uint32_t rkey() const { return rkey_; }
    uint32_t get_lkey() const { return lkey_; }
    uint32_t get_rkey() const { return rkey_; }
    uint32_t access_flags() const { return access_flags_; }

    // 内存访问权限检查
    bool can_read() const { return access_flags_ & IBV_ACCESS_REMOTE_READ; }
    bool can_write() const { return access_flags_ & IBV_ACCESS_REMOTE_WRITE; }
    bool can_atomic() const { return access_flags_ & IBV_ACCESS_REMOTE_ATOMIC; }

    // 内存操作
    bool read(void* dest, size_t offset, size_t length) const;
    bool write(const void* src, size_t offset, size_t length);
    bool atomic_cmp_swp(size_t offset, uint64_t compare, uint64_t swap, uint64_t* old_val);
    bool atomic_fetch_add(size_t offset, uint64_t add, uint64_t* old_val);

    // 缓存相关
    void update_cache_info();
    bool is_cached() const { return is_cached_; }
    void set_cached(bool cached) { is_cached_ = cached; }

    // 统计信息
    Stats get_stats() const { return stats_; }
    void reset_stats();

    // 内存保护
    bool protect(uint32_t new_access_flags);
    bool is_protected() const { return is_protected_; }

private:
    void* addr_;
    size_t length_;
    uint32_t lkey_;
    uint32_t rkey_;
    uint32_t access_flags_;
    std::atomic<bool> is_cached_{false};
    std::atomic<bool> is_protected_{false};
    mutable std::mutex mutex_;
    mutable Stats stats_;

    // 辅助函数
    bool validate_access(size_t offset, size_t length) const;
    void update_stats_read(size_t length) const;
    void update_stats_write(size_t length) const;
    void update_stats_atomic() const;
    void update_stats_cache_hit() const;
    void update_stats_cache_miss() const;
};