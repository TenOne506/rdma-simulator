#include "rdma_monitor.h"
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace std::chrono;

double RdmaMonitor::ConnectionStats::send_rate() const {
    if (send_count == 0) return 0.0;
    
    auto now = steady_clock::now();
    auto duration = duration_cast<nanoseconds>(now - last_send_time).count();
    
    if (duration < 1e-6) return 0.0; // 避免除以零
    
    return total_bytes_sent / duration;
}

double RdmaMonitor::ConnectionStats::recv_rate() const {
    if (recv_count == 0) return 0.0;
    
    auto now = steady_clock::now();
    auto duration = duration_cast<nanoseconds>(now - last_recv_time).count();
    
    if (duration < 1e-6) return 0.0; // 避免除以零
    
    return total_bytes_received / duration;
}

void RdmaMonitor::update_send_timestamp(ConnectionStats& stats, size_t size) {
    auto now = steady_clock::now();
    stats.last_send_time = now;
    stats.send_count++;
    stats.total_bytes_sent += size;
}

void RdmaMonitor::update_recv_timestamp(ConnectionStats& stats, size_t size) {
    auto now = steady_clock::now();
    stats.last_recv_time = now;
    stats.recv_count++;
    stats.total_bytes_received += size;
}

void RdmaMonitor::record_send(uint32_t qp_num, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& stats = stats_[qp_num];
    update_send_timestamp(stats, size);
    
    global_stats_.total_send_ops++;
    global_stats_.total_bytes_sent += size;
}

void RdmaMonitor::record_recv(uint32_t qp_num, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& stats = stats_[qp_num];
    update_recv_timestamp(stats, size);
    
    global_stats_.total_recv_ops++;
    global_stats_.total_bytes_received += size;
}

void RdmaMonitor::record_read(uint32_t qp_num, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& stats = stats_[qp_num];
    update_recv_timestamp(stats, size);
    stats.read_count++;
    
    global_stats_.total_read_ops++;
    global_stats_.total_bytes_received += size;
}

void RdmaMonitor::record_write(uint32_t qp_num, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& stats = stats_[qp_num];
    update_send_timestamp(stats, size);
    stats.write_count++;
    
    global_stats_.total_write_ops++;
    global_stats_.total_bytes_sent += size;
}

void RdmaMonitor::record_error(uint32_t qp_num) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    stats_[qp_num].error_count++;
    global_stats_.total_errors++;
}

void RdmaMonitor::add_connection(uint32_t qp_num) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (stats_.find(qp_num) == stats_.end()) {
        stats_[qp_num] = ConnectionStats{};
        global_stats_.total_connections++;
        global_stats_.active_connections++;
    }
}

void RdmaMonitor::remove_connection(uint32_t qp_num) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (stats_.erase(qp_num)) {
        global_stats_.active_connections--;
    }
}

RdmaMonitor::ConnectionStats RdmaMonitor::get_stats(uint32_t qp_num) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = stats_.find(qp_num);
    if (it != stats_.end()) {
        return it->second;
    }
    return ConnectionStats{};
}

RdmaMonitor::GlobalStats RdmaMonitor::get_global_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return global_stats_;
}

std::map<uint32_t, RdmaMonitor::ConnectionStats> RdmaMonitor::get_all_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void RdmaMonitor::print_summary() const {
    auto global = get_global_stats();
    
    std::cout << "\n=== RDMA Connection Summary ===" << std::endl;
    std::cout << "Total connections: " << global.total_connections << std::endl;
    std::cout << "Active connections: " << global.active_connections << std::endl;
    std::cout << "Total send operations: " << global.total_send_ops << std::endl;
    std::cout << "Total receive operations: " << global.total_recv_ops << std::endl;
    std::cout << "Total read operations: " << global.total_read_ops << std::endl;
    std::cout << "Total write operations: " << global.total_write_ops << std::endl;
    std::cout << "Total errors: " << global.total_errors << std::endl;
    std::cout << "Total bytes sent: " << global.total_bytes_sent << std::endl;
    std::cout << "Total bytes received: " << global.total_bytes_received << std::endl;
    std::cout << "==============================" << std::endl;
}

void RdmaMonitor::print_connection_stats(uint32_t qp_num) const {
    auto stats = get_stats(qp_num);
    
    std::cout << "\n=== Connection " << qp_num << " Statistics ===" << std::endl;
    std::cout << "Send operations: " << stats.send_count << std::endl;
    std::cout << "Receive operations: " << stats.recv_count << std::endl;
    std::cout << "Read operations: " << stats.read_count << std::endl;
    std::cout << "Write operations: " << stats.write_count << std::endl;
    std::cout << "Errors: " << stats.error_count << std::endl;
    std::cout << "Total bytes sent: " << stats.total_bytes_sent << std::endl;
    std::cout << "Total bytes received: " << stats.total_bytes_received << std::endl;
    std::cout << "Current send rate: " << stats.send_rate() << " bytes/sec" << std::endl;
    std::cout << "Current receive rate: " << stats.recv_rate() << " bytes/sec" << std::endl;
    std::cout << "=================================" << std::endl;
}

void RdmaMonitor::reset_stats(uint32_t qp_num) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (auto it = stats_.find(qp_num); it != stats_.end()) {
        it->second = ConnectionStats{};
    }
}

void RdmaMonitor::reset_all_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& [qp_num, stats] : stats_) {
        stats = ConnectionStats{};
    }
    
    global_stats_ = GlobalStats{};
    global_stats_.total_connections = stats_.size();
    global_stats_.active_connections = stats_.size();
}