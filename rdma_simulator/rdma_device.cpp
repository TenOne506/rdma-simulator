#include "rdma_device.h"
#include "rdma_cache.h"
#include <iostream>
#include <random>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <unistd.h>

// RDMA access flags
#define IBV_ACCESS_REMOTE_READ  0x1
#define IBV_ACCESS_REMOTE_WRITE 0x2
#define IBV_ACCESS_LOCAL_WRITE  0x4
#define IBV_ACCESS_REMOTE_ATOMIC 0x8

RdmaDevice::RdmaDevice() 
    : next_qp_num_(1), next_cq_num_(1), next_mr_key_(1), 
    stop_network_(false),
    cache_(1024 * 1024 * 64), // 64MB cache
    max_connections_(1024),   // 默认支持1024连接
    connections_mutex_(),     // 初始化互斥锁
    callbacks_mutex_(),       // 初始化互斥锁
    connection_protocol_mutex_() { // 初始化互斥锁
    std::cout << "Initializing RdmaDevice with max_connections=" 
              << max_connections_ << std::endl;
}

RdmaDevice::~RdmaDevice() {
    stop_network_thread();
    
    // 清理所有连接
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (const auto& conn : connections_) {
        if (auto mr = get_mr(conn.second.mr_key)) {
            free(mr->addr());
        }
    }
    connections_.clear();
}

uint32_t RdmaDevice::create_qp(uint32_t max_send_wr, uint32_t max_recv_wr) {
    uint32_t qp_num = next_qp_num_++;
    
    // 先检查缓存
    QPCacheValue qp_value;
    if (cache_.get_qp_info(qp_num, qp_value)) {
        // 缓存命中，直接返回缓存的QP信息
        return qp_num;
    }
    
    // 缓存未命中，创建新的QP
    qps_[qp_num] = std::make_unique<RdmaQueuePair>(qp_num, max_send_wr, max_recv_wr);
    
    // 更新缓存
    qp_value.qp_num = qp_num;
    qp_value.state.state = 0; // INIT
    qp_value.state.access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    qp_value.state.port_num = 1;
    qp_value.state.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    qp_value.pd_handle = 0; // 默认PD句柄
    qp_value.send_cq = 0;   // 默认发送CQ
    qp_value.recv_cq = 0;   // 默认接收CQ
    qp_value.last_access = std::chrono::steady_clock::now();
    qp_value.access_count = 0;
    cache_.set_qp_info(qp_num, qp_value);

    // 将QP添加到connections_映射中
    std::lock_guard<std::mutex> lock(connections_mutex_);
    Connection conn;
    conn.qp_num = qp_num;
    conn.state = QPState::INIT;
    conn.conn_state = ConnectionState::DISCONNECTED;
    conn.last_activity = std::chrono::system_clock::now();
    connections_[qp_num] = std::move(conn);
    
    return qp_num;
}

uint32_t RdmaDevice::create_cq(uint32_t max_cqe) {
    uint32_t cq_num = next_cq_num_++;
    
    // 先检查缓存
    CQCacheValue cq_value;
    if (cache_.get_cq_info(cq_num, cq_value)) {
        // 缓存命中，直接返回缓存的CQ信息
        return cq_num;
    }
    
    // 缓存未命中，创建新的CQ
    cqs_[cq_num] = std::make_unique<RdmaCompletionQueue>(cq_num, max_cqe);
    
    // 更新缓存
    cq_value.cq_num = cq_num;
    cq_value.pd_handle = 0; // 默认PD句柄
    cq_value.completions.clear();
    cq_value.last_access = std::chrono::steady_clock::now();
    cq_value.access_count = 0;
    cache_.set_cq_info(cq_num, cq_value);
    
    return cq_num;
}

uint32_t RdmaDevice::register_mr(void* addr, size_t length) {
    uint32_t lkey = next_mr_key_++;
    uint32_t rkey = next_mr_key_++;
    
    // 先检查缓存
    MRCacheValue mr_value;
    if (cache_.get_mr_info(lkey, mr_value)) {
        // 缓存命中，直接返回缓存的MR信息
        return lkey;
    }
    
    // 缓存未命中，创建新的MR
    uint32_t access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    mrs_[lkey] = std::make_unique<RdmaMemoryRegion>(addr, length, lkey, rkey, access_flags);
    
    // 更新缓存
    mr_value.lkey = lkey;
    mr_value.rkey = rkey;
    mr_value.addr = reinterpret_cast<uint64_t>(addr);
    mr_value.length = length;
    mr_value.access_flags = access_flags;
    mr_value.pd_handle = 0; // 默认PD句柄
    mr_value.blocks.clear();
    cache_.set_mr_info(lkey, mr_value);
    
    return lkey;
}

RdmaQueuePair* RdmaDevice::get_qp(uint32_t qp_num) {
    // 先检查缓存
    QPCacheValue qp_value;
    if (cache_.get_qp_info(qp_num, qp_value)) {
        // 缓存命中，返回QP
        auto it = qps_.find(qp_num);
        if (it != qps_.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

RdmaCompletionQueue* RdmaDevice::get_cq(uint32_t cq_num) {
    // 先检查缓存
    CQCacheValue cq_value;
    if (cache_.get_cq_info(cq_num, cq_value)) {
        // 缓存命中，返回CQ
        auto it = cqs_.find(cq_num);
        if (it != cqs_.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

RdmaMemoryRegion* RdmaDevice::get_mr(uint32_t lkey) {
    // 先检查缓存
    MRCacheValue mr_value;
    if (cache_.get_mr_info(lkey, mr_value)) {
        // 缓存命中，返回MR
        auto it = mrs_.find(lkey);
        if (it != mrs_.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

void RdmaDevice::start_network_thread() {
    network_thread_ = std::thread([this] { network_processing_loop(); });
}

void RdmaDevice::stop_network_thread() {
    stop_network_ = true;
    if (network_thread_.joinable()) {
        network_thread_.join();
    }
}

void RdmaDevice::network_processing_loop() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1, 100);
    
    while (!stop_network_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        for (auto& qp_pair : qps_) {
            RdmaWorkRequest wr;
            while (qp_pair.second->get_send_request(wr)) {
                bool success = dis(gen) > 5; // 95%成功率
                
                // 检查缓存
                if (wr.op_type == RdmaOpType::READ || wr.op_type == RdmaOpType::WRITE) {
                    // 查找对应的MR
                    for (const auto& mr_pair : mrs_) {
                        if (mr_pair.second->addr() == wr.remote_addr) {
                            // 检查缓存中是否有数据
                            if (cache_.contains(ComponentType::MR, mr_pair.second->lkey())) {
                                // 缓存命中，可以优化操作
                                success = true; // 缓存命中时总是成功
                            }
                            break;
                        }
                    }
                }
                
                uint32_t cq_num = qp_pair.first;
                if (auto cq = get_cq(cq_num)) {
                    RdmaCompletion comp;
                    comp.wr_id = wr.wr_id;
                    comp.success = success;
                    comp.op_type = wr.op_type;
                    comp.imm_data = wr.imm_data;
                    comp.byte_len = wr.length;
                    comp.src_qp = qp_pair.first;
                    comp.src_qp_num = qp_pair.second->get_qp_num();
                    cq->post_completion(comp);
                }
            }
        }
    }
}

void RdmaDevice::cache_put(void* addr, const void* data, size_t size, uint32_t lkey) {
    // 查找对应的MR
    for (const auto& mr_pair : mrs_) {
        if (mr_pair.second->addr() == addr) {
            // 使用MR的lkey进行缓存存储
            cache_.put(ComponentType::MR, mr_pair.second->lkey(), data, size);
            break;
        }
    }
}

bool RdmaDevice::cache_get(void* addr, void* buffer, size_t size) {
    // 查找对应的MR
    for (const auto& mr_pair : mrs_) {
        if (mr_pair.second->addr() == addr) {
            // 使用MR的lkey进行缓存查找
            return cache_.get(ComponentType::MR, mr_pair.second->lkey(), buffer, size);
        }
    }
    return false;
}

Stats RdmaDevice::get_cache_stats() const {
    return cache_.get_stats();
}

// 批量创建QP
std::vector<uint32_t> RdmaDevice::create_qp_batch(size_t count, uint32_t max_send_wr, uint32_t max_recv_wr) {
    std::vector<uint32_t> qp_nums;
    qp_nums.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        qp_nums.push_back(create_qp(max_send_wr, max_recv_wr));
    }
    
    return qp_nums;
}

// 批量创建CQ
std::vector<uint32_t> RdmaDevice::create_cq_batch(size_t count, uint32_t max_cqe) {
    std::vector<uint32_t> cq_nums;
    cq_nums.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        cq_nums.push_back(create_cq(max_cqe));
    }
    
    return cq_nums;
}

// 批量注册内存区域
std::vector<uint32_t> RdmaDevice::register_mr_batch(const std::vector<std::pair<void*, size_t>>& regions) {
    std::vector<uint32_t> mr_keys;
    mr_keys.reserve(regions.size());
    
    for (const auto& region : regions) {
        mr_keys.push_back(register_mr(region.first, region.second));
    }
    
    return mr_keys;
}

// 设置最大连接数
void RdmaDevice::set_max_connections(size_t max_conn) {
    if (max_conn == 0) {
        throw std::invalid_argument("Max connections cannot be zero");
    }
    max_connections_ = max_conn;
}

// 批量删除QP
void RdmaDevice::destroy_qp_batch(const std::vector<uint32_t>& qp_nums) {
    for (uint32_t qp_num : qp_nums) {
        qps_.erase(qp_num);
    }
}

// 批量删除CQ
void RdmaDevice::destroy_cq_batch(const std::vector<uint32_t>& cq_nums) {
    for (uint32_t cq_num : cq_nums) {
        cqs_.erase(cq_num);
    }
}

// 批量注销MR
void RdmaDevice::deregister_mr_batch(const std::vector<uint32_t>& mr_keys) {
    for (uint32_t mr_key : mr_keys) {
        mrs_.erase(mr_key);
    }
}

// 连接管理相关函数实现
std::vector<RdmaDevice::Connection> RdmaDevice::establish_connections(
    size_t count, size_t buffer_size, uint32_t max_send_wr, uint32_t max_recv_wr, uint32_t max_cqe) {
    
    std::cout << "Attempting to establish " << count << " connections" << std::endl;
    std::cout << "Current active connections: " << active_connections() << std::endl;
    std::cout << "Max connections allowed: " << max_connections_ << std::endl;

    if (active_connections() + count > max_connections_) {
        count = max_connections_ - active_connections();
        std::cout << "Adjusting connection count to " << count 
                  << " due to max connections limit" << std::endl;
        if (count == 0) {
            throw std::runtime_error("Cannot create more connections: max limit reached");
        }
    }

    std::vector<Connection> new_connections;
    new_connections.reserve(count);

    // 1. 批量创建CQ
    std::cout << "Creating " << count << " completion queues..." << std::endl;
    auto cq_nums = create_cq_batch(count, max_cqe);
    if (cq_nums.empty()) {
        throw std::runtime_error("Failed to create completion queues");
    }
    std::cout << "Successfully created " << cq_nums.size() << " CQs" << std::endl;

    // 2. 批量创建QP
    std::cout << "Creating " << count << " queue pairs..." << std::endl;
    auto qp_nums = create_qp_batch(count, max_send_wr, max_recv_wr);
    if (qp_nums.empty()) {
        destroy_cq_batch(cq_nums);
        throw std::runtime_error("Failed to create queue pairs");
    }
    std::cout << "Successfully created " << qp_nums.size() << " QPs" << std::endl;

    // 3. 分配内存区域
    std::vector<std::pair<void*, size_t>> regions;
    regions.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        void* buffer = malloc(buffer_size);
        if (!buffer) {
            // 清理已分配的资源
            for (const auto& region : regions) {
                free(region.first);
            }
            destroy_qp_batch(qp_nums);
            destroy_cq_batch(cq_nums);
            throw std::runtime_error("Failed to allocate memory for buffer");
        }
        regions.emplace_back(buffer, buffer_size);
    }

    // 4. 注册内存区域
    auto mr_keys = register_mr_batch(regions);
    if (mr_keys.empty()) {
        // 清理已分配的资源
        for (const auto& region : regions) {
            free(region.first);
        }
        destroy_qp_batch(qp_nums);
        destroy_cq_batch(cq_nums);
        throw std::runtime_error("Failed to register memory regions");
    }

    // 5. 创建连接并更新状态
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        
        size_t connections_created = 0;
        try {
            for (size_t i = 0; i < qp_nums.size(); ++i) {
                if (connections_.size() >= max_connections_) {
                    std::cout << "Warning: Reached max connections limit (" 
                              << max_connections_ << ")" << std::endl;
                    break;
                }

                Connection conn;
                conn.qp_num = qp_nums[i];
                conn.cq_num = cq_nums[i];
                conn.mr_key = mr_keys[i];
                conn.buffer = regions[i].first;
                conn.buffer_size = regions[i].second;
                conn.state = QPState::RESET;
                conn.remote_qp_num = 0;
                conn.remote_mr_key = 0;
                conn.remote_addr = nullptr;
                conn.remote_lid = 0;
                conn.conn_state = ConnectionState::DISCONNECTED;
                conn.last_activity = std::chrono::system_clock::now();
                conn.bytes_sent = 0;
                conn.bytes_received = 0;
                conn.error_count = 0;
                memset(conn.remote_gid, 0, sizeof(conn.remote_gid));

                connections_[qp_nums[i]] = std::move(conn);
                new_connections.push_back(std::move(conn));
                connections_created++;
                current_connections_++;

                // 将QP状态转换为INIT
                modify_qp_state(qp_nums[i], QPState::INIT);
            }
            
            std::cout << "Successfully created " << connections_created 
                      << " connections" << std::endl;
            
        } catch (...) {
            // 清理已创建的资源
            for (size_t i = 0; i < connections_created; ++i) {
                connections_.erase(qp_nums[i]);
                current_connections_--;
                free(regions[i].first);
            }
            deregister_mr_batch(mr_keys);
            destroy_qp_batch(qp_nums);
            destroy_cq_batch(cq_nums);
            throw;
        }
    }

    return new_connections;
}

RdmaDevice::Connection RdmaDevice::get_connection(uint32_t qp_num) const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        throw std::out_of_range("Connection with QP num " + std::to_string(qp_num) + " not found");
    }
    
    // 创建一个新的Connection对象，但不复制control_channel
    Connection conn;
    conn.qp_num = it->second.qp_num;
    conn.cq_num = it->second.cq_num;
    conn.mr_key = it->second.mr_key;
    conn.buffer = it->second.buffer;
    conn.buffer_size = it->second.buffer_size;
    conn.state = it->second.state;
    conn.remote_qp_num = it->second.remote_qp_num;
    conn.remote_mr_key = it->second.remote_mr_key;
    conn.remote_addr = it->second.remote_addr;
    conn.remote_lid = it->second.remote_lid;
    memcpy(conn.remote_gid, it->second.remote_gid, sizeof(conn.remote_gid));
    conn.conn_state = it->second.conn_state;
    conn.last_activity = it->second.last_activity;
    conn.bytes_sent = it->second.bytes_sent;
    conn.bytes_received = it->second.bytes_received;
    conn.error_count = it->second.error_count;
    conn.control_channel = nullptr;  // 不复制control_channel
    
    return conn;
}

void RdmaDevice::close_connection(uint32_t qp_num) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        throw std::out_of_range("Connection with QP num " + std::to_string(qp_num) + " not found");
    }

    // 获取需要清理的资源信息
    auto mr_key = it->second.mr_key;
    auto buffer = it->second.buffer;
    auto cq_num = it->second.cq_num;

    // 删除连接
    connections_.erase(it);

    // 清理资源
    if (auto mr = get_mr(mr_key)) {
        free(buffer);
    }
    deregister_mr_batch({mr_key});
    destroy_qp_batch({qp_num});
    destroy_cq_batch({cq_num});
}

void RdmaDevice::post_send_batch(
    const std::vector<std::pair<uint32_t, std::vector<char>>>& send_requests) {
    
    std::vector<std::pair<uint32_t, RdmaWorkRequest>> batch;
    batch.reserve(send_requests.size());

    for (const auto& req : send_requests) {
        uint32_t qp_num = req.first;
        const auto& data = req.second;

        Connection conn;
        try {
            conn = get_connection(qp_num);
            validate_qp_state(qp_num, QPState::RTS);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << ", skipping send" << std::endl;
            continue;
        }

        if (data.size() > conn.buffer_size) {
            std::cerr << "Error: Data size " << data.size() 
                      << " exceeds buffer size " << conn.buffer_size 
                      << " for QP " << qp_num << std::endl;
            continue;
        }

        memcpy(conn.buffer, data.data(), data.size());

        uint64_t wr_id = next_wr_id_++;
        RdmaWorkRequest wr{
            .wr_id = wr_id,
            .op_type = RdmaOpType::SEND,
            .local_addr = conn.buffer,
            .local_key = conn.mr_key,
            .remote_addr = nullptr,
            .remote_key = 0,
            .length = static_cast<uint32_t>(data.size()),
            .compare_add = 0,
            .swap = 0,
            .imm_data = 0,
            .signaled = true
        };

        batch.emplace_back(qp_num, wr);
    }

    if (!batch.empty()) {
        for (const auto& item : batch) {
            if (auto qp = get_qp(item.first)) {
                const auto& wr = item.second;
                qp->post_send(wr.wr_id, wr.op_type, wr.local_addr, 
                            wr.local_key, wr.remote_addr, wr.remote_key, 
                            wr.length, wr.signaled);
            }
        }
    }
}

void RdmaDevice::post_recv_batch(const std::vector<uint32_t>& qp_nums) {
    std::vector<std::pair<uint32_t, RdmaWorkRequest>> batch;
    batch.reserve(qp_nums.size());

    for (uint32_t qp_num : qp_nums) {
        Connection conn;
        try {
            conn = get_connection(qp_num);
            validate_qp_state(qp_num, QPState::RTR);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << ", skipping recv" << std::endl;
            continue;
        }

        uint64_t wr_id = next_wr_id_++;
        RdmaWorkRequest wr{
            .wr_id = wr_id,
            .op_type = RdmaOpType::RECV,
            .local_addr = conn.buffer,
            .local_key = conn.mr_key,
            .remote_addr = nullptr,
            .remote_key = 0,
            .length = static_cast<uint32_t>(conn.buffer_size),
            .compare_add = 0,
            .swap = 0,
            .imm_data = 0,
            .signaled = true
        };
        batch.emplace_back(qp_num, wr);
    }

    if (!batch.empty()) {
        for (const auto& item : batch) {
            if (auto qp = get_qp(item.first)) {
                const auto& wr = item.second;
                qp->post_recv(wr.wr_id, wr.local_addr, wr.local_key, wr.length);
            }
        }
    }
}

size_t RdmaDevice::active_connections() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_.size();
}

bool RdmaDevice::establish_rc_connection(uint32_t qp_num, 
                                       const std::string& remote_addr,
                                       uint16_t remote_port,
                                       const QPAttributes& qp_attr,
                                       uint32_t timeout_ms) {
    std::cout << "Establishing RC connection for QP " << qp_num << std::endl;
    
    try {
        // 1. 验证QP状态
        validate_qp_state(qp_num, QPState::INIT);
        
        // 2. 保存QP属性
        {
            std::lock_guard<std::mutex> lock(connection_protocol_mutex_);
            qp_attributes_[qp_num] = qp_attr;
        }
        
        // 3. 建立控制通道连接
        bool is_server = (remote_addr == "0.0.0.0");
        if (is_server) {
            if (!start_control_server(qp_num, remote_port)) {
                throw std::runtime_error("Failed to start control server");
            }
        } else {
            if (!connect_control_client(qp_num, remote_addr, remote_port)) {
                throw std::runtime_error("Failed to connect control client");
            }
        }
        
        // 4. 发送连接请求
        ConnectionMessage req_msg;
        req_msg.type = ConnectionMessageType::REQ;
        req_msg.qp_num = qp_num;
        req_msg.qp_attr = qp_attr;
        req_msg.timeout_ms = timeout_ms;
        
        if (!send_connection_message(qp_num, req_msg)) {
            throw std::runtime_error("Failed to send connection request");
        }
        
        // 5. 等待连接响应
        ConnectionMessage rep_msg;
        if (!receive_connection_message(qp_num, rep_msg, timeout_ms)) {
            throw std::runtime_error("Timeout waiting for connection response");
        }
        
        if (rep_msg.type == ConnectionMessageType::REJ) {
            handle_connection_rejection(qp_num, "Connection rejected by peer");
            return false;
        }
        
        if (!handle_connection_response(qp_num, rep_msg)) {
            throw std::runtime_error("Failed to handle connection response");
        }
        
        // 6. 发送RTR消息
        ConnectionMessage rtr_msg;
        rtr_msg.type = ConnectionMessageType::RTR;
        rtr_msg.qp_num = qp_num;
        rtr_msg.qp_attr = qp_attributes_[qp_num];
        
        if (!send_connection_message(qp_num, rtr_msg)) {
            throw std::runtime_error("Failed to send RTR message");
        }
        
        // 7. 等待RTS消息
        ConnectionMessage rts_msg;
        if (!receive_connection_message(qp_num, rts_msg, timeout_ms)) {
            throw std::runtime_error("Timeout waiting for RTS message");
        }
        
        if (!handle_rts_message(qp_num, rts_msg)) {
            throw std::runtime_error("Failed to handle RTS message");
        }
        
        // 8. 验证远程QP
        if (!verify_remote_qp(qp_num, rts_msg)) {
            throw std::runtime_error("Failed to verify remote QP");
        }
        
        // 9. 转换QP状态到RTS
        modify_qp_state(qp_num, QPState::RTS);
        
        // 10. 更新连接状态
        update_connection_state(qp_num, ConnectionState::CONNECTED);
        
        std::cout << "RC connection established successfully for QP " << qp_num << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        update_connection_state(qp_num, ConnectionState::ERROR);
        notify_error(qp_num, std::string("Connection establishment failed: ") + e.what());
        return false;
    }
}

bool RdmaDevice::handle_connection_request(uint32_t qp_num, 
                                         const ConnectionMessage& req_msg) {
    std::lock_guard<std::mutex> lock(connection_protocol_mutex_);
    
    // 1. 验证QP属性
    if (!verify_qp_attributes(qp_attributes_[qp_num], req_msg.qp_attr)) {
        handle_connection_rejection(qp_num, "QP attributes mismatch");
        return false;
    }
    
    // 2. 更新远程QP信息
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        return false;
    }
    
    it->second.remote_qp_num = req_msg.qp_num;
    it->second.remote_mr_key = req_msg.remote_mr_key;
    it->second.remote_addr = reinterpret_cast<void*>(req_msg.remote_addr);
    it->second.remote_lid = req_msg.remote_lid;
    memcpy(it->second.remote_gid, req_msg.remote_gid, sizeof(req_msg.remote_gid));
    
    // 3. 发送连接响应
    ConnectionMessage rep_msg;
    rep_msg.type = ConnectionMessageType::REP;
    rep_msg.qp_num = qp_num;
    rep_msg.qp_attr = qp_attributes_[qp_num];
    
    return send_connection_message(qp_num, rep_msg);
}

bool RdmaDevice::handle_connection_response(uint32_t qp_num, 
                                          const ConnectionMessage& rep_msg) {
    std::lock_guard<std::mutex> lock(connection_protocol_mutex_);
    
    // 1. 验证QP属性
    if (!verify_qp_attributes(qp_attributes_[qp_num], rep_msg.qp_attr)) {
        handle_connection_rejection(qp_num, "QP attributes mismatch");
        return false;
    }
    
    // 2. 更新远程QP信息
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        return false;
    }
    
    it->second.remote_qp_num = rep_msg.qp_num;
    it->second.remote_mr_key = rep_msg.remote_mr_key;
    it->second.remote_addr = reinterpret_cast<void*>(rep_msg.remote_addr);
    it->second.remote_lid = rep_msg.remote_lid;
    memcpy(it->second.remote_gid, rep_msg.remote_gid, sizeof(rep_msg.remote_gid));
    
    // 3. 转换QP状态到RTR
    modify_qp_state(qp_num, QPState::RTR);
    
    return true;
}

bool RdmaDevice::handle_rtr_message(uint32_t qp_num, 
                                   const ConnectionMessage& rtr_msg) {
    std::lock_guard<std::mutex> lock(connection_protocol_mutex_);
    
    // 1. 验证QP属性
    if (!verify_qp_attributes(qp_attributes_[qp_num], rtr_msg.qp_attr)) {
        handle_connection_rejection(qp_num, "QP attributes mismatch");
        return false;
    }
    
    // 2. 转换QP状态到RTR
    modify_qp_state(qp_num, QPState::RTR);
    
    // 3. 发送RTS消息
    ConnectionMessage rts_msg;
    rts_msg.type = ConnectionMessageType::RTS;
    rts_msg.qp_num = qp_num;
    rts_msg.qp_attr = qp_attributes_[qp_num];
    
    return send_connection_message(qp_num, rts_msg);
}

bool RdmaDevice::handle_rts_message(uint32_t qp_num, 
                                   const ConnectionMessage& rts_msg) {
    std::lock_guard<std::mutex> lock(connection_protocol_mutex_);
    
    // 1. 验证QP属性
    if (!verify_qp_attributes(qp_attributes_[qp_num], rts_msg.qp_attr)) {
        handle_connection_rejection(qp_num, "QP attributes mismatch");
        return false;
    }
    
    // 2. 转换QP状态到RTS
    modify_qp_state(qp_num, QPState::RTS);
    
    return true;
}

bool RdmaDevice::verify_qp_attributes(const QPAttributes& local_attr,
                                    const QPAttributes& remote_attr) {
    // 验证QP属性是否兼容
    if (local_attr.max_send_wr < remote_attr.max_recv_wr ||
        local_attr.max_recv_wr < remote_attr.max_send_wr ||
        local_attr.max_send_sge < remote_attr.max_recv_sge ||
        local_attr.max_recv_sge < remote_attr.max_send_sge) {
        return false;
    }
    
    // 验证其他属性
    if (local_attr.max_rd_atomic < remote_attr.max_dest_rd_atomic ||
        local_attr.max_dest_rd_atomic < remote_attr.max_rd_atomic) {
        return false;
    }
    
    return true;
}

bool RdmaDevice::verify_remote_qp(uint32_t qp_num, 
                                 const ConnectionMessage& msg) {
    // 在实际实现中，这里应该验证远程QP是否可达
    // 可以通过发送测试消息来验证
    return true;
}

void RdmaDevice::handle_connection_rejection(uint32_t qp_num, 
                                           const std::string& reason) {
    ConnectionMessage rej_msg;
    rej_msg.type = ConnectionMessageType::REJ;
    rej_msg.qp_num = qp_num;
    
    send_connection_message(qp_num, rej_msg);
    update_connection_state(qp_num, ConnectionState::ERROR);
    notify_error(qp_num, "Connection rejected: " + reason);
}

bool RdmaDevice::retry_connection(uint32_t qp_num, 
                                 const ConnectionMessage& msg,
                                 uint32_t max_retries) {
    std::lock_guard<std::mutex> lock(connection_protocol_mutex_);
    
    auto& retry_count = connection_retry_counts_[qp_num];
    if (retry_count >= max_retries) {
        return false;
    }
    
    retry_count++;
    return send_connection_message(qp_num, msg);
}

bool RdmaDevice::send_connection_message(uint32_t qp_num, 
                                       const ConnectionMessage& msg) {
    auto it = connections_.find(qp_num);
    if (it == connections_.end() || !it->second.control_channel) {
        return false;
    }
    
    // 根据消息类型选择不同的发送方法
    switch (msg.type) {
        case ConnectionMessageType::REQ: {
            RdmaQPInfo qp_info;
            qp_info.qp_num = msg.qp_num;
            qp_info.qp_access_flags = msg.qp_attr.qp_access_flags;
            qp_info.pkey_index = msg.qp_attr.pkey_index;
            qp_info.port_num = msg.qp_attr.port_num;
            memcpy(qp_info.gid, msg.remote_gid, sizeof(qp_info.gid));
            return it->second.control_channel->send_connect_request(qp_info);
        }
        case ConnectionMessageType::REP: {
            RdmaQPInfo qp_info;
            qp_info.qp_num = msg.qp_num;
            qp_info.qp_access_flags = msg.qp_attr.qp_access_flags;
            qp_info.pkey_index = msg.qp_attr.pkey_index;
            qp_info.port_num = msg.qp_attr.port_num;
            memcpy(qp_info.gid, msg.remote_gid, sizeof(qp_info.gid));
            return it->second.control_channel->send_connect_response(qp_info, true);
        }
        case ConnectionMessageType::RTR:
        case ConnectionMessageType::RTS:
            return it->second.control_channel->send_ready();
        case ConnectionMessageType::REJ:
            return it->second.control_channel->send_error("Connection rejected");
        default:
            return false;
    }
}

bool RdmaDevice::receive_connection_message(uint32_t qp_num, 
                                          ConnectionMessage& msg,
                                          uint32_t timeout_ms) {
    auto it = connections_.find(qp_num);
    if (it == connections_.end() || !it->second.control_channel) {
        return false;
    }
    
    // 接收控制消息
    RdmaControlMsg ctrl_msg;
    if (!it->second.control_channel->receive_message(ctrl_msg, timeout_ms)) {
        return false;
    }
    
    // 根据消息类型设置ConnectionMessage
    switch (ctrl_msg.header.type) {
        case RdmaControlMsgType::CONNECT_REQ:
            msg.type = ConnectionMessageType::REQ;
            if (ctrl_msg.payload.size() >= sizeof(RdmaQPInfo)) {
                const RdmaQPInfo* qp_info = reinterpret_cast<const RdmaQPInfo*>(ctrl_msg.payload.data());
                msg.qp_num = qp_info->qp_num;
                msg.qp_attr.qp_access_flags = qp_info->qp_access_flags;
                msg.qp_attr.pkey_index = qp_info->pkey_index;
                msg.qp_attr.port_num = qp_info->port_num;
                memcpy(msg.remote_gid, qp_info->gid, sizeof(msg.remote_gid));
            }
            break;
        case RdmaControlMsgType::CONNECT_RESP:
            msg.type = ConnectionMessageType::REP;
            if (ctrl_msg.payload.size() >= sizeof(RdmaQPInfo)) {
                const RdmaQPInfo* qp_info = reinterpret_cast<const RdmaQPInfo*>(ctrl_msg.payload.data());
                msg.qp_num = qp_info->qp_num;
                msg.qp_attr.qp_access_flags = qp_info->qp_access_flags;
                msg.qp_attr.pkey_index = qp_info->pkey_index;
                msg.qp_attr.port_num = qp_info->port_num;
                memcpy(msg.remote_gid, qp_info->gid, sizeof(msg.remote_gid));
            }
            break;
        case RdmaControlMsgType::READY:
            msg.type = ConnectionMessageType::RTS;  // 假设READY消息对应RTS状态
            break;
        case RdmaControlMsgType::ERROR:
            msg.type = ConnectionMessageType::REJ;
            break;
        default:
            return false;
    }
    
    return true;
}

void RdmaDevice::modify_qp_state(uint32_t qp_num, QPState new_state) {
    QPState current_state;
    {
        //std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(qp_num);
        if (it == connections_.end()) {
            throw std::out_of_range("QP not found");
        }
        current_state = it->second.state;
    }

    // 在锁外执行状态转换
    transition_qp_state(qp_num, current_state, new_state);

    // 更新状态
    {
        //std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(qp_num);
        if (it != connections_.end()) {
            it->second.state = new_state;
        }
    }
}

void RdmaDevice::transition_qp_state(uint32_t qp_num, QPState from_state, QPState to_state) {
    if (auto qp = get_qp(qp_num)) {
        switch (to_state) {
            case QPState::INIT:
                if (from_state != QPState::RESET) {
                    throw std::runtime_error("Invalid state transition to INIT");
                }
                if (!qp->modify_to_init()) {
                    throw std::runtime_error("Failed to modify QP to INIT state");
                }
                break;
            case QPState::RTR:
                if (from_state != QPState::INIT) {
                    throw std::runtime_error("Invalid state transition to RTR");
                }
                if (!qp->modify_to_rtr(0, 0, 0)) { // 临时使用默认值，实际应该从连接信息中获取
                    throw std::runtime_error("Failed to modify QP to RTR state");
                }
                break;
            case QPState::RTS:
                if (from_state != QPState::RTR) {
                    throw std::runtime_error("Invalid state transition to RTS");
                }
                if (!qp->modify_to_rts()) {
                    throw std::runtime_error("Failed to modify QP to RTS state");
                }
                break;
            default:
                throw std::runtime_error("Invalid target state");
        }
    }
}

void RdmaDevice::set_remote_qp_info(uint32_t qp_num, 
                                   uint32_t remote_qp_num,
                                   uint32_t remote_mr_key,
                                   void* remote_addr,
                                   uint16_t remote_lid,
                                   const uint8_t* remote_gid) {
    bool need_state_transition = false;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(qp_num);
        if (it == connections_.end()) {
            throw std::out_of_range("QP not found");
        }

        it->second.remote_qp_num = remote_qp_num;
        it->second.remote_mr_key = remote_mr_key;
        it->second.remote_addr = remote_addr;
        it->second.remote_lid = remote_lid;
        if (remote_gid) {
            memcpy(it->second.remote_gid, remote_gid, 16);
        }

        // 检查是否需要状态转换
        need_state_transition = (it->second.state == QPState::INIT);
    }

    // 在锁外执行状态转换
    if (need_state_transition) {
        modify_qp_state(qp_num, QPState::RTR);
    }
}

QPState RdmaDevice::get_qp_state(uint32_t qp_num) const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        throw std::out_of_range("QP not found");
    }
    return it->second.state;
}

void RdmaDevice::validate_qp_state(uint32_t qp_num, QPState required_state) const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        throw std::out_of_range("QP not found");
    }
    if (it->second.state != required_state) {
        throw std::runtime_error("Invalid QP state for operation");
    }
}

void RdmaDevice::register_connection_callback(ConnectionCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    connection_callbacks_.push_back(callback);
}

void RdmaDevice::register_data_callback(DataCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    data_callbacks_.push_back(callback);
}

void RdmaDevice::register_error_callback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    error_callbacks_.push_back(callback);
}

ConnectionState RdmaDevice::get_connection_state(uint32_t qp_num) const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        throw std::out_of_range("QP not found");
    }
    return it->second.conn_state;
}

bool RdmaDevice::send_data(uint32_t qp_num, const void* data, size_t length, bool should_wait_for_completion) {
    try {
        validate_qp_state(qp_num, QPState::RTS);
        
        Connection conn = get_connection(qp_num);
        if (length > conn.buffer_size) {
            throw std::runtime_error("Data size exceeds buffer size");
        }

        // 复制数据到缓冲区
        memcpy(conn.buffer, data, length);

        // 发送数据
        uint64_t wr_id = get_next_wr_id();
        if (auto qp = get_qp(qp_num)) {
            qp->post_send(wr_id, RdmaOpType::SEND, conn.buffer, conn.mr_key, 
                         nullptr, 0, length, true);
        }

        // 如果需要等待完成
        if (should_wait_for_completion) {
            if (!wait_for_completion(qp_num, 5000)) {
                throw std::runtime_error("Timeout waiting for send completion");
            }
        }

        // 更新统计信息
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(qp_num);
            if (it != connections_.end()) {
                it->second.bytes_sent += length;
                it->second.last_activity = std::chrono::system_clock::now();
            }
        }

        return true;
    } catch (const std::exception& e) {
        notify_error(qp_num, std::string("Send data failed: ") + e.what());
        return false;
    }
}

bool RdmaDevice::receive_data(uint32_t qp_num, void* buffer, size_t buffer_size, 
                            size_t& received_length, uint32_t timeout_ms) {
    try {
        validate_qp_state(qp_num, QPState::RTR);
        
        Connection conn = get_connection(qp_num);
        if (buffer_size > conn.buffer_size) {
            throw std::runtime_error("Buffer size exceeds connection buffer size");
        }

        // 等待接收完成
        if (!wait_for_completion(qp_num, timeout_ms)) {
            throw std::runtime_error("Timeout waiting for receive completion");
        }

        // 复制数据到用户缓冲区
        received_length = std::min(buffer_size, conn.buffer_size);
        memcpy(buffer, conn.buffer, received_length);

        // 更新统计信息
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(qp_num);
            if (it != connections_.end()) {
                it->second.bytes_received += received_length;
                it->second.last_activity = std::chrono::system_clock::now();
            }
        }

        // 通知数据接收回调
        notify_data_received(qp_num, buffer, received_length);

        return true;
    } catch (const std::exception& e) {
        notify_error(qp_num, std::string("Receive data failed: ") + e.what());
        return false;
    }
}

void RdmaDevice::update_connection_state(uint32_t qp_num, ConnectionState new_state) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(qp_num);
    if (it != connections_.end()) {
        it->second.conn_state = new_state;
        notify_connection_state_change(qp_num, new_state);
    }
}

void RdmaDevice::notify_connection_state_change(uint32_t qp_num, ConnectionState state) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    for (const auto& callback : connection_callbacks_) {
        callback(qp_num, state);
    }
}

void RdmaDevice::notify_data_received(uint32_t qp_num, const void* data, size_t length) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    for (const auto& callback : data_callbacks_) {
        callback(qp_num, data, length);
    }
}

void RdmaDevice::notify_error(uint32_t qp_num, const std::string& error) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    for (const auto& callback : error_callbacks_) {
        callback(qp_num, error);
    }
}

bool RdmaDevice::wait_for_completion(uint32_t qp_num, uint32_t timeout_ms) {
    auto start_time = std::chrono::steady_clock::now();
    while (true) {
        if (poll_completion(qp_num, 100)) {  // 每100ms检查一次
            return true;
        }

        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - start_time).count();
        if (elapsed >= timeout_ms) {
            return false;
        }
    }
}

bool RdmaDevice::poll_completion(uint32_t qp_num, uint32_t timeout_ms) {
    Connection conn = get_connection(qp_num);
    if (auto cq = get_cq(conn.cq_num)) {
        RdmaCompletion comp;
        if (cq->poll_completion(comp, std::chrono::milliseconds(timeout_ms))) {
            return comp.success;
        }
    }
    return false;
}

bool RdmaDevice::establish_rdma_connection(uint32_t qp_num, 
                                         const std::string& remote_addr,
                                         uint16_t remote_port,
                                         uint32_t timeout_ms) {
    // 使用默认QP属性
    QPAttributes qp_attr;
    qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.max_send_wr = 16;
    qp_attr.max_recv_wr = 16;
    qp_attr.max_send_sge = 1;
    qp_attr.max_recv_sge = 1;
    qp_attr.max_rd_atomic = 1;
    qp_attr.max_dest_rd_atomic = 1;
    
    return establish_rc_connection(qp_num, remote_addr, remote_port, qp_attr, timeout_ms);
}

bool RdmaDevice::start_control_server(uint32_t qp_num, uint16_t port) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        return false;
    }
    
    // 创建控制通道
    it->second.control_channel = std::make_unique<RdmaControlChannel>();
    
    // 启动服务器
    if (!it->second.control_channel->start_server(port)) {
        it->second.control_channel.reset();
        return false;
    }
    
    return true;
}

bool RdmaDevice::connect_control_client(uint32_t qp_num, 
                                      const std::string& server_ip, 
                                      uint16_t port) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        return false;
    }
    
    // 创建控制通道
    it->second.control_channel = std::make_unique<RdmaControlChannel>();
    
    // 连接到服务器
    if (!it->second.control_channel->connect_to_server(server_ip, port)) {
        it->second.control_channel.reset();
        return false;
    }
    
    return true;
}

bool RdmaDevice::init_qp(uint32_t qp_num, uint32_t cq_num) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    
    // 检查QP是否存在
    auto it = connections_.find(qp_num);
    if (it == connections_.end()) {
        return false;
    }

    // 检查CQ是否存在
    auto cq = get_cq(cq_num);
    if (!cq) {
        return false;
    }

    // 获取QP
    auto qp = get_qp(qp_num);
    if (!qp) {
        return false;
    }

    // 将QP状态修改为INIT
    if (!qp->modify_to_init()) {
        return false;
    }

    // 更新连接信息
    it->second.cq_num = cq_num;
    it->second.state = QPState::INIT;
    it->second.last_activity = std::chrono::system_clock::now();

    return true;
}
