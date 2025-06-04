#include "rdma_connection_manager.h"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <algorithm>

RdmaConnectionManager::RdmaConnectionManager(RdmaDevice& device, size_t max_connections)
    : device_(device), max_connections_(max_connections) {
    if (max_connections == 0) {
        throw std::invalid_argument("Max connections cannot be zero");
    }
}

std::vector<RdmaConnectionManager::Connection> RdmaConnectionManager::establish_connections(
    size_t count, size_t buffer_size, uint32_t max_send_wr, uint32_t max_recv_wr, uint32_t max_cqe) {
    
    std::vector<Connection> new_connections;
    new_connections.reserve(count);

    // 1. 批量创建CQ (每个连接一个CQ)
    auto cq_nums = device_.create_cq_batch(count, max_cqe);
    if (cq_nums.empty()) {
        throw std::runtime_error("Failed to create any completion queues");
    }

    // 2. 批量创建QP (每个连接一个QP)
    auto qp_nums = device_.create_qp_batch(count, max_send_wr, max_recv_wr);
    if (qp_nums.empty()) {
        // 回滚已创建的CQ
        device_.destroy_cq_batch(cq_nums);
        throw std::runtime_error("Failed to create any queue pairs");
    }

    // 3. 准备内存区域
    std::vector<std::pair<void*, size_t>> regions;
    try {
        regions.reserve(qp_nums.size());
        for (size_t i = 0; i < qp_nums.size(); ++i) {
            void* buffer = malloc(buffer_size);
            if (!buffer) {
                throw std::bad_alloc();
            }
            memset(buffer, 0, buffer_size);
            regions.emplace_back(buffer, buffer_size);
        }
    } catch (...) {
        // 清理已分配的内存
        for (auto& region : regions) {
            free(region.first);
        }
        device_.destroy_qp_batch(qp_nums);
        device_.destroy_cq_batch(cq_nums);
        throw;
    }

    // 4. 批量注册MR
    auto mr_keys = device_.register_mr_batch(regions);
    if (mr_keys.size() != qp_nums.size()) {
        // 部分失败，清理资源
        for (size_t i = mr_keys.size(); i < regions.size(); ++i) {
            free(regions[i].first);
        }
        device_.deregister_mr_batch(mr_keys);
        device_.destroy_qp_batch(qp_nums);
        device_.destroy_cq_batch(cq_nums);
        throw std::runtime_error("Failed to register all memory regions");
    }

    // 5. 创建连接结构
    std::lock_guard<std::mutex> lock(connections_mutex_);
    
    size_t connections_created = 0;
    try {
        for (size_t i = 0; i < qp_nums.size(); ++i) {
            if (connections_.size() >= max_connections_) {
                std::cerr << "Warning: Reached max connections limit (" 
                          << max_connections_ << ")" << std::endl;
                break;
            }

            Connection conn{
                .qp_num = qp_nums[i],
                .cq_num = cq_nums[i],
                .mr_key = mr_keys[i],
                .buffer = regions[i].first,
                .buffer_size = regions[i].second
            };

            connections_[qp_nums[i]] = conn;
            new_connections.push_back(conn);
            connections_created++;
        }
    } catch (...) {
        // 回滚已添加的连接
        for (size_t i = 0; i < connections_created; ++i) {
            connections_.erase(qp_nums[i]);
            free(regions[i].first);
        }
        device_.deregister_mr_batch(mr_keys);
        device_.destroy_qp_batch(qp_nums);
        device_.destroy_cq_batch(cq_nums);
        throw;
    }

    return new_connections;
}

RdmaConnectionManager::Connection RdmaConnectionManager::get_connection(uint32_t qp_num) const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        throw std::out_of_range("Connection with QP num " + std::to_string(qp_num) + " not found");
    }
    
    return it->second;
}

void RdmaConnectionManager::close_connection(uint32_t qp_num) {
    Connection conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(qp_num);
        if (it == connections_.end()) {
            throw std::out_of_range("Connection with QP num " + std::to_string(qp_num) + " not found");
        }
        conn = it->second;
        connections_.erase(it);
    }

    // 清理资源 (按照创建的反顺序)
    if (auto mr = device_.get_mr(conn.mr_key)) {
        free(mr->addr()); // 释放内存缓冲区
    }
    // device_.deregister_mr(conn.mr_key);
    // device_.destroy_qp(qp_num);
    // device_.destroy_cq(conn.cq_num);
}

void RdmaConnectionManager::post_send_batch(
    const std::vector<std::pair<uint32_t, std::vector<char>>>& send_requests) {
    
    std::vector<std::pair<uint32_t, RdmaQueuePair::WorkRequest>> batch;
    batch.reserve(send_requests.size());

    for (const auto& req : send_requests) {
        uint32_t qp_num = req.first;
        const auto& data = req.second;

        Connection conn;
        try {
            conn = get_connection(qp_num);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << ", skipping send" << std::endl;
            continue;
        }

        // 验证数据大小
        if (data.size() > conn.buffer_size) {
            std::cerr << "Error: Data size " << data.size() 
                      << " exceeds buffer size " << conn.buffer_size 
                      << " for QP " << qp_num << std::endl;
            continue;
        }

        // 拷贝数据到缓冲区
        memcpy(conn.buffer, data.data(), data.size());

        // 准备发送请求
        RdmaQueuePair::WorkRequest wr{
            .wr_id = qp_num, // 使用QP号作为工作请求ID
            .op_type = RdmaOpType::SEND,
            .local_addr = conn.buffer,
            .lkey = conn.mr_key,
            .remote_addr = nullptr,
            .rkey = 0,
            .length = static_cast<uint32_t>(data.size())
        };

        batch.emplace_back(qp_num, wr);
    }

    // 批量提交发送请求
    if (!batch.empty()) {
        // 使用设备级的批量处理接口
        for (const auto& item : batch) {
            if (auto qp = device_.get_qp(item.first)) {
                const auto& wr = item.second;
                qp->post_send(wr.wr_id, wr.op_type, wr.local_addr, 
                            wr.lkey, wr.remote_addr, wr.rkey, wr.length);
            }
        }
    }
}

void RdmaConnectionManager::post_recv_batch(const std::vector<uint32_t>& qp_nums) {
    std::vector<std::pair<uint32_t, RdmaQueuePair::WorkRequest>> batch;
    batch.reserve(qp_nums.size());

    for (uint32_t qp_num : qp_nums) {
        Connection conn;
        try {
            conn = get_connection(qp_num);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << ", skipping recv" << std::endl;
            continue;
        }

        // 准备接收请求
        // RdmaDevice::WorkRequest wr{
        //     .wr_id = qp_num,
        //     .op_type = RdmaOpType::RECV,
        //     .local_addr = conn.buffer,
        //     .lkey = conn.mr_key,
        //     .remote_addr = nullptr,
        //     .rkey = 0,
        //     .length = static_cast<uint32_t>(conn.buffer_size)
        // };
        RdmaQueuePair::WorkRequest wr{
            .wr_id = qp_num,
            .op_type = RdmaOpType::RECV,
            .local_addr = conn.buffer,
            .lkey = conn.mr_key,
            .remote_addr = nullptr,
            .rkey = 0,
            .length = static_cast<uint32_t>(conn.buffer_size)
        };
        batch.emplace_back(qp_num, wr);
    }

    // 批量提交接收请求
    if (!batch.empty()) {
        // 使用设备级的批量处理接口
        for (const auto& item : batch) {
            if (auto qp = device_.get_qp(item.first)) {
                const auto& wr = item.second;
                qp->post_recv(wr.wr_id, wr.local_addr, wr.lkey, wr.length);
            }
        }
    }
}

size_t RdmaConnectionManager::active_connections() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_.size();
}