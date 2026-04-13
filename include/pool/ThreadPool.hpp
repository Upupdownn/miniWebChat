#pragma once

#include <atomic>
#include <thread>
#include <functional>
#include <chrono>
#include <iostream>
#include <future>
#include <algorithm>
#include <vector>
#include <utility>
#include <stdexcept>
#include <memory>
#include <cstddef>

#include "BlockingQueue.hpp"


class ThreadPool
{
private:

    const size_t core_threads_;     // 最小线程数（基本盘）
    const size_t max_threads_;      // 最大线程数（除去核心线程，剩下非核心线程可以动态创建/销毁）
    BlockingQueue<std::function<void()>> task_queue_;       // 任务队列
    const std::chrono::milliseconds manager_interval_;      // 管理者线程的巡视间隔
    const std::chrono::milliseconds idle_timeout_;          // 线程允许的空闲时间

    std::atomic<bool> running_;
    std::atomic<bool> accepting_tasks_;    // TODO：实现双标志，不关闭线程池但拒绝添加任务

    std::atomic<size_t> live_threads_;      // 存活的线程数
    std::atomic<size_t> busy_threads_;      // 工作的线程数（<= live_threads_）
    std::atomic<size_t> completed_tasks_;   // 已完成的任务数 TODO：优化为记录每个线程完成的任务数

    std::thread manager_;               // 管理线程
    std::vector<std::thread> workers_;  // 工作线程
    std::mutex workers_mtx_;
    std::vector<std::thread::id> exited_thread_ids_;
    std::mutex exited_mtx_;

    std::atomic<size_t> thread_id_seed_;        // 线程 id（自增）
    
public:
    ThreadPool(
        size_t core_threads, 
        size_t max_threads, 
        size_t queue_capacity = 0,
        std::chrono::milliseconds manager_interval = std::chrono::milliseconds(1000),
        std::chrono::milliseconds idle_timeout = std::chrono::milliseconds(3000))
        :   core_threads_(core_threads),
            max_threads_(max_threads),
            task_queue_(queue_capacity),
            manager_interval_(manager_interval),
            idle_timeout_(idle_timeout),
            running_(true),
            accepting_tasks_(true),
            live_threads_(0),
            busy_threads_(0),
            completed_tasks_(0),
            thread_id_seed_(0)
    {
        // 检查参数
        if (core_threads_ == 0)
            throw std::invalid_argument("core_threads must be > 0");
        if (max_threads_ < core_threads_)
            throw std::invalid_argument("max_threads must be >= core_threads");

        for (size_t i = 0; i < core_threads_; ++i)
            create_worker();

        manager_ = std::thread(&ThreadPool::manager_loop, this);
    }
    ~ThreadPool() {
        shutdown();
    }

    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

public:
    // 提交任务
    template<typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args)
        -> std::future<typename std::invoke_result<Func, Args...>::type>;

    // 关闭线程池
    void shutdown();

    size_t core_threads() const { return core_threads_; }

    size_t max_threads() const { return max_threads_; }

    size_t live_threads() const { return live_threads_.load(); }

    size_t busy_threads() const { return busy_threads_.load(); }

    size_t pending_tasks() const { return task_queue_.size(); }

    size_t completed_tasks() const { return completed_tasks_.load(); }

private:
    // 创建工作线程，执行 worker_loop
    void create_worker();

    // 工作线程入口
    void worker_loop(size_t worker_id);

    // 工作线程执行任务
    void execute_task(std::function<void()>& task); // TODO：增加 worker_id

    // 管理线程入口
    void manager_loop();
};

// 提交任务
template<typename Func, typename... Args>
auto ThreadPool::submit(Func&& func, Args&&... args)
    -> std::future<typename std::invoke_result<Func, Args...>::type>
{
    using ReturnType = typename std::invoke_result<Func, Args...>::type;

    if (!accepting_tasks_.load())
        throw std::runtime_error("thread pool can't accepting new tasks");

    auto packaged_task_ptr = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
    );

    std::future<ReturnType> result_future = packaged_task_ptr->get_future();

    std::function<void()> task_wrapper = [packaged_task_ptr](){
        (*packaged_task_ptr)();
    };

    if (!task_queue_.push(std::move(task_wrapper)))
        throw std::runtime_error("failed to submit task");

    return result_future;
}

void ThreadPool::
create_worker()
{
    std::lock_guard<std::mutex> lock(workers_mtx_);

    // 注意：成员函数隐含 this 指针参数
    workers_.emplace_back(&ThreadPool::worker_loop, this, ++thread_id_seed_);
    live_threads_.fetch_add(1);
}

void ThreadPool::
worker_loop(size_t worker_id)
{
    (void)worker_id;
    
    // 线程打卡（即下班时间，注意：不是上班时间）
    auto last_active_time = std::chrono::steady_clock::now();

    // 线程循环
    while (1)
    {
        std::function<void()> task;

        // 拿出任务并执行
        if (task_queue_.pop_for(task, std::chrono::milliseconds(200)))
        {
            execute_task(task);
            last_active_time = std::chrono::steady_clock::now();
            continue;
        }

        // 线程池关闭 且 任务队列空
        if (!running_.load() && task_queue_.empty())
            break;

        // 当前存活的线程 <= 核心线程，不用销毁
        if (live_threads_.load() <= core_threads_)
            continue;
        
        // 存活的线程 > 核心线程，但空闲时间 < 阈值，不用销毁
        auto now = std::chrono::steady_clock::now();
        auto idle_duration = 
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_active_time
            );
        if (idle_duration < idle_timeout_)
            continue;

        // 线程自毁
        size_t curr_live = live_threads_.load();

        // 循环判断，直到自毁成功 或 其他线程自毁使得没有非核心线程
        while (curr_live > core_threads_)
        {
            if (live_threads_.compare_exchange_weak(curr_live, curr_live - 1))
            {
                std::lock_guard<std::mutex> lock(exited_mtx_);
                exited_thread_ids_.push_back(std::this_thread::get_id());
                return;
            }
        }
    }
    live_threads_.fetch_sub(1);
}

void ThreadPool::
execute_task(std::function<void()>& task)
{
    busy_threads_.fetch_add(1);

    try
    {
        task();
    }
    catch(const std::exception& e)  // 抛出标准异常
    {
        std::cerr << "Standard exception: " << e.what() << std::endl;
    }
    catch (...)     // 兜底（防止抛出奇怪的东西）
    {
        std::cerr << "Unknown critical error caught in worker thread!" << std::endl;
    }
    
    busy_threads_.fetch_sub(1);
    completed_tasks_.fetch_add(1);
}

void ThreadPool::
manager_loop()
{
    while (running_.load())
    {
        std::this_thread::sleep_for(manager_interval_);

        if (!running_.load())
            break;

        size_t pending = task_queue_.size();
        size_t live = live_threads_.load();
        size_t busy = busy_threads_.load();

        // 先确定是否需要扩容
        bool need_expand = false;
        if (live > 0 && busy >= live && pending > 0)    // 所有线程都在工作
            need_expand = true;
        if (pending > live)     // 任务激增
            need_expand = true;

        // 扩容不能超过最大线程数
        if (need_expand && live < max_threads_)
        {
            // 最大扩容数
            size_t can_create = max_threads_ - live;

            // 期望扩容（普通情况1，任务激增则最大2）
            size_t grow_count = pending > live ? std::min((size_t)2, (pending - live)) : 1;

            // 不超过最大限制
            grow_count = std::min(grow_count, can_create);
            
            for (size_t i = 0; i < grow_count; i++)
                create_worker();
        }

        // ----- 收尸自毁的线程 -----

        // 拿出自毁的线程id
        std::vector<std::thread::id> ids_to_reap;
        {
            std::lock_guard<std::mutex> lock(exited_mtx_);
            if (!exited_thread_ids_.empty())
                ids_to_reap.swap(exited_thread_ids_);
        }
        if (ids_to_reap.empty())
            continue;

        // 根据 id 找到位于 workers_ 的线程
        std::lock_guard<std::mutex> lock(workers_mtx_);
        for (auto id : ids_to_reap)
        {
            auto it = std::find_if(workers_.begin(), workers_.end(), [id](const std::thread& t){
                return t.get_id() == id;
            });
            if (it == workers_.end())
                continue;

            if (it->joinable())
                it->join();

            workers_.erase(it);
        }
    }
}

void ThreadPool::
shutdown()
{
    // 线程池只关一次
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
        return;

    // 停止添加新的任务
    accepting_tasks_.store(false);

    // 关闭任务队列
    task_queue_.shutdown();
    
    // 回收管理线程
    if (manager_.joinable())
        manager_.join();

    // 回收工作线程
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        for (std::thread& worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
        workers_.clear();
    }
}
