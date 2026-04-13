#pragma once

#include <mutex>
#include <condition_variable>
#include <concepts>
#include <atomic>
#include <queue>

template <typename T>
class BlockingQueue
{
public:
    explicit BlockingQueue(size_t capacity = 0): capacity_(capacity), isshutdown_(false) {}
    ~BlockingQueue() {shutdown();}

    // 禁止拷贝
    BlockingQueue(const BlockingQueue& ) = delete;
    BlockingQueue& operator=(const BlockingQueue& ) = delete;

    // 入队
    // template<typename U>
    // requires std::is_assignable_v<T&, U&&>      // 确保 U 可以赋值给 T
    bool push(const T& value)
    {
        std::unique_lock<std::mutex> lock(mtx_);

        cond_not_full_.wait(lock, [this](){
            return isshutdown_ || capacity_ == 0 || queue_.size() < capacity_;
        });

        if (isshutdown_) return false;
        
        queue_.push(value);
        cond_not_empty_.notify_one();

        return true;
    }

    bool push(T&& value)
    {
        std::unique_lock<std::mutex> lock(mtx_);

        cond_not_full_.wait(lock, [this](){
            return isshutdown_ || capacity_ == 0 || queue_.size() < capacity_;
        });

        if (isshutdown_) return false;
        
        queue_.push(std::move(value));
        cond_not_empty_.notify_one();

        return true;
    }

    // 出队（阻塞式）
    bool pop(T& value)
    {
        std::unique_lock<std::mutex> lock(mtx_);

        cond_not_empty_.wait(lock, [this](){
            return isshutdown_ || !queue_.empty();
        });

        if (isshutdown_ && queue_.empty())
            return false;

        value = std::move(queue_.front());
        queue_.pop();

        cond_not_full_.notify_one();
        return true;
    }

    // 出队（带超时）
    bool pop_for(T& value, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mtx_);

        if (!cond_not_empty_.wait_for(lock, timeout, [this](){
            return isshutdown_ || !queue_.empty();
        }))
            return false;

        if (isshutdown_ && queue_.empty())
            return false;

        value = std::move(queue_.front());
        queue_.pop();

        cond_not_full_.notify_one();
        return true;
    }

    // 出队（非阻塞式）
    bool try_pop(T& value)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        if (queue_.empty())
            return false;

        value = std::move(queue_.front());
        queue_.pop();

        cond_not_full_.notify_one();
        return true;
    }

    // 关闭队列
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            isshutdown_ = true;
        }
        cond_not_empty_.notify_all();
        cond_not_full_.notify_all();
    }
    
    // 获得队列大小
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }
    
    // 是否为空
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

private:
    // 线程安全控制
    mutable std::mutex mtx_;
    std::condition_variable cond_not_empty_;
    std::condition_variable cond_not_full_;

    std::queue<T> queue_;               // 数据队列
    size_t capacity_;                   // 队列最大容量
    std::atomic<bool> isshutdown_;        // 队列关闭标志
};
