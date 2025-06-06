#ifndef RDMA_DEVICE_H
#define RDMA_DEVICE_H

#include <memory>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include "rdma_cache.h"
#include "rdma_monitor.h"
#include "rdma_queue_pair.h"
#include "rdma_completion_queue.h"
#include "rdma_memory_region.h"

// Forward declarations
struct RdmaWorkRequest;
struct RdmaCompletion;

/**
 * @brief RDMA设备类，模拟RDMA网卡(RNIC)的功能
 * 
 * 该类负责管理RDMA资源，包括队列对(QP)、完成队列(CQ)和内存区域(MR)。
 * 同时提供网络处理线程和缓存系统，用于优化RDMA操作性能。
 */
class RdmaDevice {
public:
    /**
     * @brief 构造函数，初始化RDMA设备
     * 
     * 初始化资源计数器、创建缓存系统，并准备网络处理线程
     */
    RdmaDevice();

    /**
     * @brief 析构函数，清理RDMA设备资源
     * 
     * 停止网络处理线程，释放所有已分配的RDMA资源
     */
    ~RdmaDevice();
    
    /**
     * @brief 创建队列对(Queue Pair)
     * 
     * @param max_send_wr 发送工作请求队列的最大长度
     * @param max_recv_wr 接收工作请求队列的最大长度
     * @return uint32_t 返回新创建的QP编号
     */
    uint32_t create_qp(uint32_t max_send_wr, uint32_t max_recv_wr);

    /**
     * @brief 创建完成队列(Completion Queue)
     * 
     * @param max_cqe 完成队列的最大条目数
     * @return uint32_t 返回新创建的CQ编号
     */
    uint32_t create_cq(uint32_t max_cqe);

    /**
     * @brief 注册内存区域(Memory Region)
     * 
     * @param addr 要注册的内存区域起始地址
     * @param length 要注册的内存区域长度
     * @return uint32_t 返回新注册的MR的本地密钥(lkey)
     */
    uint32_t register_mr(void* addr, size_t length);
    
    /**
     * @brief 获取指定编号的队列对
     * 
     * @param qp_num QP编号
     * @return RdmaQueuePair* 返回QP指针，如果不存在则返回nullptr
     */
    RdmaQueuePair* get_qp(uint32_t qp_num);

    /**
     * @brief 获取指定编号的完成队列
     * 
     * @param cq_num CQ编号
     * @return RdmaCompletionQueue* 返回CQ指针，如果不存在则返回nullptr
     */
    RdmaCompletionQueue* get_cq(uint32_t cq_num);

    /**
     * @brief 获取指定本地密钥的内存区域
     * 
     * @param lkey 内存区域的本地密钥
     * @return RdmaMemoryRegion* 返回MR指针，如果不存在则返回nullptr
     */
    RdmaMemoryRegion* get_mr(uint32_t lkey);
    
    /**
     * @brief 启动网络处理线程
     * 
     * 创建并启动后台线程，用于处理RDMA网络请求和完成事件。
     * 该线程会持续运行直到调用stop_network_thread()
     */
    void start_network_thread();

    /**
     * @brief 停止网络处理线程
     * 
     * 安全地停止网络处理线程，确保所有pending的操作都被正确处理
     */
    void stop_network_thread();

    /**
     * @brief 将数据写入缓存
     * 
     * @param addr 目标内存地址
     * @param data 源数据指针
     * @param size 数据大小
     * @param lkey 内存区域的本地密钥
     */
    void cache_put(void* addr, const void* data, size_t size, uint32_t lkey);

    /**
     * @brief 从缓存读取数据
     * 
     * @param addr 源内存地址
     * @param buffer 目标缓冲区
     * @param size 要读取的数据大小
     * @return bool 如果缓存命中返回true，否则返回false
     */
    bool cache_get(void* addr, void* buffer, size_t size);

    /**
     * @brief 获取缓存统计信息
     * 
     * @return RdmaCache::Stats 返回包含命中率等统计信息的结构体
     */
    RdmaCache::Stats get_cache_stats() const;
    
    // 批量创建QP
    std::vector<uint32_t> create_qp_batch(size_t count, uint32_t max_send_wr, uint32_t max_recv_wr);
    
    // 批量创建CQ
    std::vector<uint32_t> create_cq_batch(size_t count, uint32_t max_cqe);
    
    // 批量注册内存区域
    std::vector<uint32_t> register_mr_batch(const std::vector<std::pair<void*, size_t>>& regions);
    
    // 获取设备连接数
    size_t connection_count() const;
    
    // 设置最大连接数
    void set_max_connections(size_t max_conn);

    // 批量删除QP
    void destroy_qp_batch(const std::vector<uint32_t>& qp_nums);
    
    // 批量删除CQ
    void destroy_cq_batch(const std::vector<uint32_t>& cq_nums);
    
    // 批量注销MR
    void deregister_mr_batch(const std::vector<uint32_t>& mr_keys);
    
     // 添加监控接口
    const RdmaMonitor& get_monitor() const { return monitor_; }
    RdmaMonitor& get_monitor() { return monitor_; }
    
private:
    /**
     * @brief 网络处理循环函数
     * 
     * 在单独的线程中运行，负责处理RDMA网络请求、
     * 生成完成事件并更新相关资源状态
     */
    void network_processing_loop();

    uint32_t next_qp_num_;      ///< 下一个可用的QP编号
    uint32_t next_cq_num_;      ///< 下一个可用的CQ编号
    uint32_t next_mr_key_;      ///< 下一个可用的MR密钥
    std::map<uint32_t, std::unique_ptr<RdmaQueuePair>> qps_;        ///< 队列对映射表，键为QP编号
    std::map<uint32_t, std::unique_ptr<RdmaCompletionQueue>> cqs_;  ///< 完成队列映射表，键为CQ编号
    std::map<uint32_t, std::unique_ptr<RdmaMemoryRegion>> mrs_;     ///< 内存区域映射表，键为本地密钥
    std::thread network_thread_;  ///< 网络处理线程
    bool stop_network_;           ///< 网络线程停止标志

    RdmaCache cache_;  ///< RDMA缓存系统，用于优化内存访问性能

    size_t max_connections_ = 1024; // 默认支持1024连接
    std::atomic<size_t> current_connections_{0};

    RdmaMonitor monitor_;  ///< RDMA操作监控系统
};

#endif // RDMA_DEVICE_H