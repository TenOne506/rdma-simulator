#include "rdma_memory_region.h"

RdmaMemoryRegion::RdmaMemoryRegion(void* addr, size_t length, uint32_t lkey, uint32_t rkey)
    : addr_(addr), length_(length), lkey_(lkey), rkey_(rkey) {}

void* RdmaMemoryRegion::addr() const { return addr_; }
size_t RdmaMemoryRegion::length() const { return length_; }
uint32_t RdmaMemoryRegion::lkey() const { return lkey_; }
uint32_t RdmaMemoryRegion::rkey() const { return rkey_; }

uint32_t RdmaMemoryRegion::get_lkey() const { return lkey_; }
uint32_t RdmaMemoryRegion::get_rkey() const { return rkey_; }