#pragma once

#include "RedisConnection.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

struct RedisPoolConfig
{
    RedisConfig redis_config;

    std::size_t min_connections = 2;
    std::size_t max_connections = 10;
};

class RedisConnectionPool
{
public:
    class PooledConnection
    {
    public:
        PooledConnection() = default;
        PooledConnection(RedisConnection* conn, RedisConnectionPool* pool);
        ~PooledConnection();

        PooledConnection(const PooledConnection&) = delete;
        PooledConnection& operator=(const PooledConnection&) = delete;

        PooledConnection(PooledConnection&& other) noexcept;
        PooledConnection& operator=(PooledConnection&& other) noexcept;

        RedisConnection* operator->() const;
        RedisConnection& operator*() const;
        RedisConnection* get() const;
        bool valid() const;

    private:
        void release();

    private:
        RedisConnection* conn_ = nullptr;
        RedisConnectionPool* pool_ = nullptr;
    };

public:
    explicit RedisConnectionPool(const RedisPoolConfig& config);
    ~RedisConnectionPool();

    RedisConnectionPool(const RedisConnectionPool&) = delete;
    RedisConnectionPool& operator=(const RedisConnectionPool&) = delete;

    bool init();
    PooledConnection acquire();

    std::size_t totalConnectionCount() const;
    std::size_t idleConnectionCount() const;

private:
    void releaseConnection(RedisConnection* conn);

private:
    RedisPoolConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::vector<std::unique_ptr<RedisConnection>> all_connections_;
    std::queue<RedisConnection*> idle_connections_;

    bool initialized_ = false;
    bool shutting_down_ = false;
};