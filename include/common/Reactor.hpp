#pragma once

#include <functional>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>


class Reactor {
public:
    // 事件到达时的回调函数
    struct EventHandler 
    {
        std::function<void()> read_callback;
        std::function<void()> write_callback;
        std::function<void()> error_callback;
    };

private:
    int epoll_fd_;      // Reactor 的核心成员
    bool use_ET_;       // 是否使用 ET 模式
    bool running_;      // Reactor 运行标志
    std::vector<struct epoll_event> events_;            // epoll 的事件 list
    std::unordered_map<int, EventHandler> handlers_;    // fd -> handler 的字典

public:
    Reactor(bool use_ET = false)
        : epoll_fd_(-1), use_ET_(use_ET), running_(false), events_(1024) {}
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

public:
    // 初始化（创建 epoll）
    bool init();

    // 停用（不可以再使用 loop 等 wait epoll）
    void stop();

    // epoll_wait
    void loop(int timeout_ms = 1000);

    // --- fd 管理函数 ---
    bool add_fd(int fd, uint32_t events, const EventHandler& handler);  // 添加新的 fd
    bool mod_fd(int fd, uint32_t events);   // 修改 fd 的 events
    bool del_fd(int fd);    // 删除 fd

    // 拿到 epoll 的 fd
    int epoll_fd() const;
};


Reactor::~Reactor()
{
    if (epoll_fd_ >= 0) 
    {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

bool Reactor::init()
{
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0)
        return false;

    running_ = true;

    return true;
}

void Reactor::stop()
{
    running_ = false;
}

void Reactor::loop(int timeout_ms)
{
    while (running_)
    {
        int n = ::epoll_wait(epoll_fd_, events_.data(), events_.size(), timeout_ms);
        
        if (n < 0 && errno != EINTR)
        {
            perror("epoll wait err");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            // 拿到 fd 和 事件
            int fd = events_[i].data.fd;
            uint32_t evs = events_[i].events;

            // 拿到 fd 的回调函数
            auto it = handlers_.find(fd);
            if (it == handlers_.end())
                continue;
            EventHandler& handler = it->second;

            // --- 根据事件调用回调函数 ---
            // 出错
            if (evs & (EPOLLERR | EPOLLHUP))
            {
                if (handler.error_callback)
                    handler.error_callback();
                continue;
            }
            
            // 可读
            if ((evs & EPOLLIN) && handler.read_callback)
                handler.read_callback();

            // 可写
            if ((evs & EPOLLOUT) && handler.write_callback)
                handler.write_callback();
        }
    }
}

bool Reactor::
add_fd(int fd, uint32_t events, const EventHandler& handler)
{
    struct epoll_event ev;
    ev.events = use_ET_ ? (events | EPOLLET) : events;
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        return false;

    handlers_[fd] = handler;

    return true;
}

bool Reactor::mod_fd(int fd, uint32_t events) 
{
    struct epoll_event ev;
    ev.events = use_ET_ ? (events | EPOLLET) : events;
    ev.data.fd = fd;

    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool Reactor::del_fd(int fd) 
{
    handlers_.erase(fd);

    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int Reactor::epoll_fd() const 
{
    return epoll_fd_;
}