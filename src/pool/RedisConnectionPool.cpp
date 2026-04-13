#include "RedisConnectionPool.h"

#include <stdexcept>
#include <utility>

RedisConnectionPool::PooledConnection::PooledConnection(
    RedisConnection* conn,
    RedisConnectionPool* pool)
    : conn_(conn), pool_(pool)
{
}

RedisConnectionPool::PooledConnection::~PooledConnection()
{
    release();
}

RedisConnectionPool::PooledConnection::PooledConnection(PooledConnection&& other) noexcept
    : conn_(other.conn_), pool_(other.pool_)
{
    other.conn_ = nullptr;
    other.pool_ = nullptr;
}

RedisConnectionPool::PooledConnection&
RedisConnectionPool::PooledConnection::operator=(PooledConnection&& other) noexcept
{
    if (this != &other)
    {
        release();
        conn_ = other.conn_;
        pool_ = other.pool_;
        other.conn_ = nullptr;
        other.pool_ = nullptr;
    }
    return *this;
}

RedisConnection* RedisConnectionPool::PooledConnection::operator->() const
{
    return conn_;
}

RedisConnection& RedisConnectionPool::PooledConnection::operator*() const
{
    return *conn_;
}

RedisConnection* RedisConnectionPool::PooledConnection::get() const
{
    return conn_;
}

bool RedisConnectionPool::PooledConnection::valid() const
{
    return conn_ != nullptr;
}

void RedisConnectionPool::PooledConnection::release()
{
    if (conn_ != nullptr && pool_ != nullptr)
    {
        pool_->releaseConnection(conn_);
        conn_ = nullptr;
        pool_ = nullptr;
    }
}

RedisConnectionPool::RedisConnectionPool(const RedisPoolConfig& config)
    : config_(config)
{
}

RedisConnectionPool::~RedisConnectionPool()
{
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;

    while (!idle_connections_.empty())
    {
        idle_connections_.pop();
    }

    all_connections_.clear();
    initialized_ = false;
}

bool RedisConnectionPool::init()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_)
    {
        return true;
    }

    if (config_.min_connections == 0 ||
        config_.max_connections == 0 ||
        config_.min_connections > config_.max_connections)
    {
        return false;
    }

    all_connections_.reserve(config_.max_connections);

    for (std::size_t i = 0; i < config_.min_connections; ++i)
    {
        std::unique_ptr<RedisConnection> conn = std::make_unique<RedisConnection>();
        if (!conn->connect(config_.redis_config))
        {
            return false;
        }

        RedisConnection* raw = conn.get();
        all_connections_.push_back(std::move(conn));
        idle_connections_.push(raw);
    }

    initialized_ = true;
    return true;
}

RedisConnectionPool::PooledConnection RedisConnectionPool::acquire()
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (!initialized_)
    {
        throw std::runtime_error("RedisConnectionPool is not initialized");
    }

    while (true)
    {
        if (shutting_down_)
        {
            throw std::runtime_error("RedisConnectionPool is shutting down");
        }

        if (!idle_connections_.empty())
        {
            RedisConnection* conn = idle_connections_.front();
            idle_connections_.pop();

            if (!conn->ping())
            {
                conn->reconnect();
            }

            if (conn->isConnected())
            {
                return PooledConnection(conn, this);
            }

            continue;
        }

        if (all_connections_.size() < config_.max_connections)
        {
            std::unique_ptr<RedisConnection> conn = std::make_unique<RedisConnection>();
            if (conn->connect(config_.redis_config))
            {
                RedisConnection* raw = conn.get();
                all_connections_.push_back(std::move(conn));
                return PooledConnection(raw, this);
            }
        }

        cv_.wait(lock);
    }
}

void RedisConnectionPool::releaseConnection(RedisConnection* conn)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (shutting_down_)
    {
        return;
    }

    if (conn != nullptr && conn->isConnected())
    {
        idle_connections_.push(conn);
        cv_.notify_one();
    }
}

std::size_t RedisConnectionPool::totalConnectionCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return all_connections_.size();
}

std::size_t RedisConnectionPool::idleConnectionCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return idle_connections_.size();
}