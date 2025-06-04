#ifndef RDMA_MEMORY_REGION_H
#define RDMA_MEMORY_REGION_H

#include <cstddef>
#include <cstdint>
class RdmaMemoryRegion {
public:
    RdmaMemoryRegion(void* addr, size_t length, uint32_t lkey, uint32_t rkey);
    
    void* addr() const;
    size_t length() const;
    uint32_t lkey() const;
    uint32_t rkey() const;

private:
    void* addr_;
    size_t length_;
    uint32_t lkey_;
    uint32_t rkey_;
};

#endif // RDMA_MEMORY_REGION_H