#ifndef RDMA_THREAD_POOL_H
#define RDMA_THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <atomic>

class RdmaThreadPool {
public:
    explicit RdmaThreadPool(size_t threads = std::thread::hardware_concurrency());
    ~RdmaThreadPool();
    
    // 禁止拷贝和移动
    RdmaThreadPool(const RdmaThreadPool&) = delete;
    RdmaThreadPool& operator=(const RdmaThreadPool&) = delete;
    RdmaThreadPool(RdmaThreadPool&&) = delete;
    RdmaThreadPool& operator=(RdmaThreadPool&&) = delete;
    
    // 提交任务到线程池
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>;
    
    // 获取活动任务数
    size_t active_tasks() const { return active_count_; }
    
    // 获取线程数
    size_t thread_count() const { return workers_.size(); }
    
    // 等待所有任务完成
    void wait_all();
    
    // 调整线程池大小
    void resize(size_t new_size);

private:
    // 工作线程函数
    void worker_loop();
    
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::condition_variable completion_condition_;
    
    std::atomic<bool> stop_{false};
    std::atomic<size_t> active_count_{0};
    std::atomic<size_t> total_tasks_{0};
};

// 模板实现必须在头文件中
template<class F, class... Args>
auto RdmaThreadPool::enqueue(F&& f, Args&&... args) 
    -> std::future<typename std::result_of<F(Args...)>::type> {
    
    using return_type = typename std::result_of<F(Args...)>::type;
    
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        
        if(stop_) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }
        
        tasks_.emplace([task](){ (*task)(); });
        total_tasks_++;
    }
    
    condition_.notify_one();
    return res;
}

#endif // RDMA_THREAD_POOL_H