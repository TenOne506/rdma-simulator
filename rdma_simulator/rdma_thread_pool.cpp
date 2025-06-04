#include "rdma_thread_pool.h"
#include <stdexcept>

RdmaThreadPool::RdmaThreadPool(size_t threads) {
    if(threads == 0) {
        threads = std::thread::hardware_concurrency();
    }
    
    workers_.reserve(threads);
    for(size_t i = 0; i < threads; ++i) {
        workers_.emplace_back(&RdmaThreadPool::worker_loop, this);
    }
}

RdmaThreadPool::~RdmaThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    
    condition_.notify_all();
    for(std::thread &worker: workers_) {
        if(worker.joinable()) {
            worker.join();
        }
    }
}

void RdmaThreadPool::worker_loop() {
    while(true) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this]{
                return stop_ || !tasks_.empty();
            });
            
            if(stop_ && tasks_.empty()) {
                return;
            }
            
            task = std::move(tasks_.front());
            tasks_.pop();
            active_count_++;
        }
        
        task();
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            active_count_--;
            total_tasks_--;
            
            if(total_tasks_ == 0) {
                completion_condition_.notify_all();
            }
        }
    }
}

void RdmaThreadPool::wait_all() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    completion_condition_.wait(lock, [this]{
        return tasks_.empty() && active_count_ == 0;
    });
}

void RdmaThreadPool::resize(size_t new_size) {
    if(new_size == workers_.size()) {
        return;
    }
    
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    
    condition_.notify_all();
    for(std::thread &worker: workers_) {
        if(worker.joinable()) {
            worker.join();
        }
    }
    
    workers_.clear();
    stop_ = false;
    
    workers_.reserve(new_size);
    for(size_t i = 0; i < new_size; ++i) {
        workers_.emplace_back(&RdmaThreadPool::worker_loop, this);
    }
}