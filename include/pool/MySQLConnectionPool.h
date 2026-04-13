#pragma once

#include "MySQLConnection.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

struct MySQLPoolConfig
{
    MySQLConfig db_config;

    std::size_t min_connections = 2;
    std::size_t max_connections = 10;
};

class MySQLConnectionPool
{
public:
    class PooledConnection
    {
    public:
        PooledConnection() = default;
        PooledConnection(MySQLConnection* conn, MySQLConnectionPool* pool);
        ~PooledConnection();

        PooledConnection(const PooledConnection&) = delete;
        PooledConnection& operator=(const PooledConnection&) = delete;

        PooledConnection(PooledConnection&& other) noexcept;
        PooledConnection& operator=(PooledConnection&& other) noexcept;

        MySQLConnection* operator->() const;
        MySQLConnection& operator*() const;
        MySQLConnection* get() const;
        bool valid() const;

    private:
        void release();

    private:
        MySQLConnection* conn_ = nullptr;
        MySQLConnectionPool* pool_ = nullptr;
    };

public:
    explicit MySQLConnectionPool(const MySQLPoolConfig& config);
    ~MySQLConnectionPool();

    MySQLConnectionPool(const MySQLConnectionPool&) = delete;
    MySQLConnectionPool& operator=(const MySQLConnectionPool&) = delete;

    bool init();
    PooledConnection acquire();

    std::size_t totalConnectionCount() const;
    std::size_t idleConnectionCount() const;

private:
    void releaseConnection(MySQLConnection* conn);

private:
    MySQLPoolConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::vector<std::unique_ptr<MySQLConnection>> all_connections_;
    std::queue<MySQLConnection*> idle_connections_;

    bool initialized_ = false;
    bool shutting_down_ = false;
};