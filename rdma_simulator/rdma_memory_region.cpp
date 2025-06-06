#include "rdma_memory_region.h"
#include <cstring>
#include <stdexcept>

RdmaMemoryRegion::RdmaMemoryRegion(void* addr, size_t length, uint32_t lkey, uint32_t rkey, uint32_t access_flags)
    : addr_(addr),
      length_(length),
      lkey_(lkey),
      rkey_(rkey),
      access_flags_(access_flags) {
    if (!addr || length == 0) {
        throw std::invalid_argument("Invalid memory region parameters");
    }
}

RdmaMemoryRegion::~RdmaMemoryRegion() {
    // 清理资源
    std::lock_guard<std::mutex> lock(mutex_);
    addr_ = nullptr;
    length_ = 0;
    lkey_ = 0;
    rkey_ = 0;
    access_flags_ = 0;
}

bool RdmaMemoryRegion::read(void* dest, size_t offset, size_t length) const {
    if (!can_read()) {
        return false;
    }

    if (!validate_access(offset, length)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (is_protected_) {
        return false;
    }

    memcpy(dest, static_cast<char*>(addr_) + offset, length);
    update_stats_read(length);
    return true;
}

bool RdmaMemoryRegion::write(const void* src, size_t offset, size_t length) {
    if (!can_write()) {
        return false;
    }

    if (!validate_access(offset, length)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (is_protected_) {
        return false;
    }

    memcpy(static_cast<char*>(addr_) + offset, src, length);
    update_stats_write(length);
    return true;
}

bool RdmaMemoryRegion::atomic_cmp_swp(size_t offset, uint64_t compare, uint64_t swap, uint64_t* old_val) {
    if (!can_atomic()) {
        return false;
    }

    if (offset + sizeof(uint64_t) > length_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (is_protected_) {
        return false;
    }

    uint64_t* target = static_cast<uint64_t*>(addr_) + offset;
    *old_val = *target;
    
    if (*target == compare) {
        *target = swap;
        update_stats_atomic();
        return true;
    }
    return false;
}

bool RdmaMemoryRegion::atomic_fetch_add(size_t offset, uint64_t add, uint64_t* old_val) {
    if (!can_atomic()) {
        return false;
    }

    if (offset + sizeof(uint64_t) > length_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (is_protected_) {
        return false;
    }

    uint64_t* target = static_cast<uint64_t*>(addr_) + offset;
    *old_val = *target;
    *target += add;
    
    update_stats_atomic();
    return true;
}

void RdmaMemoryRegion::update_cache_info() {
    is_cached_ = true;
}

void RdmaMemoryRegion::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
}

bool RdmaMemoryRegion::protect(uint32_t new_access_flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_protected_) {
        return false;
    }
    
    access_flags_ = new_access_flags;
    is_protected_ = true;
    return true;
}

bool RdmaMemoryRegion::validate_access(size_t offset, size_t length) const {
    if (!addr_ || length == 0) {
        return false;
    }
    
    if (offset >= length_ || length > length_ - offset) {
        return false;
    }
    
    return true;
}

void RdmaMemoryRegion::update_stats_read(size_t length) const {
    stats_.read_ops++;
    stats_.read_bytes += length;
}

void RdmaMemoryRegion::update_stats_write(size_t length) const {
    stats_.write_ops++;
    stats_.write_bytes += length;
}

void RdmaMemoryRegion::update_stats_atomic() const {
    stats_.atomic_ops++;
}

void RdmaMemoryRegion::update_stats_cache_hit() const {
    stats_.cache_hits++;
}

void RdmaMemoryRegion::update_stats_cache_miss() const {
    stats_.cache_misses++;
}

// void* RdmaMemoryRegion::addr() const { return addr_; }
// size_t RdmaMemoryRegion::length() const { return length_; }
// uint32_t RdmaMemoryRegion::lkey() const { return lkey_; }
// uint32_t RdmaMemoryRegion::rkey() const { return rkey_; }

// uint32_t RdmaMemoryRegion::get_lkey() const { return lkey_; }
// uint32_t RdmaMemoryRegion::get_rkey() const { return rkey_; }