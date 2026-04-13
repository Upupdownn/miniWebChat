#pragma once

#include <hiredis/hiredis.h>

#include <cstdint>
#include <string>
#include <vector>

struct RedisConfig
{
    std::string host = "127.0.0.1";
    uint16_t    port = 6379;

    int connect_timeout_ms = 3000;
    int socket_timeout_ms  = 3000;

    std::string password;
    int db = 0;
};

class RedisReplyWrapper
{
public:
    RedisReplyWrapper() = default;
    explicit RedisReplyWrapper(redisReply* reply);
    ~RedisReplyWrapper();

    RedisReplyWrapper(const RedisReplyWrapper&) = delete;
    RedisReplyWrapper& operator=(const RedisReplyWrapper&) = delete;

    RedisReplyWrapper(RedisReplyWrapper&& other) noexcept;
    RedisReplyWrapper& operator=(RedisReplyWrapper&& other) noexcept;

    bool valid() const;
    int type() const;

    bool isString() const;
    bool isStatus() const;
    bool isInteger() const;
    bool isArray() const;
    bool isNil() const;
    bool isError() const;

    std::string str() const;
    long long integer() const;
    std::size_t elements() const;
    std::string elementString(std::size_t index) const;

    redisReply* raw() const;

private:
    redisReply* reply_ = nullptr;
};

class RedisConnection
{
public:
    RedisConnection();
    explicit RedisConnection(const RedisConfig& config);
    ~RedisConnection();

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    RedisConnection(RedisConnection&& other) noexcept;
    RedisConnection& operator=(RedisConnection&& other) noexcept;

public:
    bool connect(const RedisConfig& config);
    void disconnect();
    bool reconnect();
    bool ping();
    bool isConnected() const;

    RedisReplyWrapper command(const std::string& cmd);

    bool set(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string* value);
    bool del(const std::string& key, std::uint64_t* deleted_count = nullptr);
    bool exists(const std::string& key, bool* exists_flag);
    bool expire(const std::string& key, int seconds);

    bool hset(const std::string& key, const std::string& field, const std::string& value);
    bool hget(const std::string& key, const std::string& field, std::string* value);

    const std::string& lastError() const;
    int lastErrorCode() const;

    redisContext* raw();

private:
    bool authIfNeeded();
    bool selectDBIfNeeded();
    void resetError();
    void setError(int code, const std::string& msg);
    void setErrorFromContext();
    void moveFrom(RedisConnection& other);

private:
    redisContext* ctx_ = nullptr;
    RedisConfig config_;
    bool connected_ = false;

    std::string last_error_;
    int last_error_code_ = 0;
};