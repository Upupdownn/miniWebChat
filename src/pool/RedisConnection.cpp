#include "RedisConnection.h"

#include <utility>

RedisReplyWrapper::RedisReplyWrapper(redisReply* reply)
    : reply_(reply)
{
}

RedisReplyWrapper::~RedisReplyWrapper()
{
    if (reply_ != nullptr)
    {
        freeReplyObject(reply_);
        reply_ = nullptr;
    }
}

RedisReplyWrapper::RedisReplyWrapper(RedisReplyWrapper&& other) noexcept
    : reply_(other.reply_)
{
    other.reply_ = nullptr;
}

RedisReplyWrapper& RedisReplyWrapper::operator=(RedisReplyWrapper&& other) noexcept
{
    if (this != &other)
    {
        if (reply_ != nullptr)
        {
            freeReplyObject(reply_);
        }
        reply_ = other.reply_;
        other.reply_ = nullptr;
    }
    return *this;
}

bool RedisReplyWrapper::valid() const
{
    return reply_ != nullptr;
}

int RedisReplyWrapper::type() const
{
    return reply_ ? reply_->type : REDIS_REPLY_NIL;
}

bool RedisReplyWrapper::isString() const
{
    return reply_ && reply_->type == REDIS_REPLY_STRING;
}

bool RedisReplyWrapper::isStatus() const
{
    return reply_ && reply_->type == REDIS_REPLY_STATUS;
}

bool RedisReplyWrapper::isInteger() const
{
    return reply_ && reply_->type == REDIS_REPLY_INTEGER;
}

bool RedisReplyWrapper::isArray() const
{
    return reply_ && reply_->type == REDIS_REPLY_ARRAY;
}

bool RedisReplyWrapper::isNil() const
{
    return reply_ && reply_->type == REDIS_REPLY_NIL;
}

bool RedisReplyWrapper::isError() const
{
    return reply_ && reply_->type == REDIS_REPLY_ERROR;
}

std::string RedisReplyWrapper::str() const
{
    if (reply_ == nullptr || reply_->str == nullptr)
    {
        return "";
    }
    return std::string(reply_->str, reply_->len);
}

long long RedisReplyWrapper::integer() const
{
    return reply_ ? reply_->integer : 0;
}

std::size_t RedisReplyWrapper::elements() const
{
    return reply_ ? reply_->elements : 0;
}

std::string RedisReplyWrapper::elementString(std::size_t index) const
{
    if (reply_ == nullptr || reply_->type != REDIS_REPLY_ARRAY || index >= reply_->elements)
    {
        return "";
    }

    redisReply* elem = reply_->element[index];
    if (elem == nullptr || elem->str == nullptr)
    {
        return "";
    }

    return std::string(elem->str, elem->len);
}

redisReply* RedisReplyWrapper::raw() const
{
    return reply_;
}

RedisConnection::RedisConnection() = default;

RedisConnection::RedisConnection(const RedisConfig& config)
{
    connect(config);
}

RedisConnection::~RedisConnection()
{
    disconnect();
}

RedisConnection::RedisConnection(RedisConnection&& other) noexcept
{
    moveFrom(other);
}

RedisConnection& RedisConnection::operator=(RedisConnection&& other) noexcept
{
    if (this != &other)
    {
        disconnect();
        moveFrom(other);
    }
    return *this;
}

void RedisConnection::moveFrom(RedisConnection& other)
{
    ctx_ = other.ctx_;
    config_ = other.config_;
    connected_ = other.connected_;
    last_error_ = std::move(other.last_error_);
    last_error_code_ = other.last_error_code_;

    other.ctx_ = nullptr;
    other.connected_ = false;
    other.last_error_.clear();
    other.last_error_code_ = 0;
}

void RedisConnection::resetError()
{
    last_error_.clear();
    last_error_code_ = 0;
}

void RedisConnection::setError(int code, const std::string& msg)
{
    last_error_code_ = code;
    last_error_ = msg;
}

void RedisConnection::setErrorFromContext()
{
    if (ctx_ != nullptr)
    {
        last_error_code_ = ctx_->err;
        last_error_ = ctx_->errstr ? ctx_->errstr : "unknown redis error";
    }
    else
    {
        last_error_code_ = -1;
        last_error_ = "redis context is null";
    }
}

bool RedisConnection::connect(const RedisConfig& config)
{
    disconnect();

    config_ = config;
    resetError();

    struct timeval tv;
    tv.tv_sec = config_.connect_timeout_ms / 1000;
    tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;

    ctx_ = redisConnectWithTimeout(config_.host.c_str(), config_.port, tv);
    if (ctx_ == nullptr)
    {
        setError(-1, "redisConnectWithTimeout returned null");
        return false;
    }

    if (ctx_->err)
    {
        setErrorFromContext();
        disconnect();
        return false;
    }

    struct timeval socket_tv;
    socket_tv.tv_sec = config_.socket_timeout_ms / 1000;
    socket_tv.tv_usec = (config_.socket_timeout_ms % 1000) * 1000;

    if (redisSetTimeout(ctx_, socket_tv) != REDIS_OK)
    {
        setErrorFromContext();
        disconnect();
        return false;
    }

    if (!authIfNeeded())
    {
        disconnect();
        return false;
    }

    if (!selectDBIfNeeded())
    {
        disconnect();
        return false;
    }

    connected_ = true;
    return true;
}

void RedisConnection::disconnect()
{
    if (ctx_ != nullptr)
    {
        redisFree(ctx_);
        ctx_ = nullptr;
    }

    connected_ = false;
}

bool RedisConnection::reconnect()
{
    return connect(config_);
}

bool RedisConnection::authIfNeeded()
{
    if (config_.password.empty())
    {
        return true;
    }

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "AUTH %s", config_.password.c_str()))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    if (!reply.isStatus() || reply.str() != "OK")
    {
        setError(-1, "AUTH failed");
        return false;
    }

    return true;
}

bool RedisConnection::selectDBIfNeeded()
{
    if (config_.db <= 0)
    {
        return true;
    }

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "SELECT %d", config_.db))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    if (!reply.isStatus() || reply.str() != "OK")
    {
        setError(-1, "SELECT db failed");
        return false;
    }

    return true;
}

bool RedisConnection::ping()
{
    if (!isConnected())
    {
        setError(-1, "connection not established");
        return false;
    }

    resetError();

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "PING"))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        connected_ = false;
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        connected_ = false;
        return false;
    }

    if ((reply.isStatus() || reply.isString()) && reply.str() == "PONG")
    {
        return true;
    }

    setError(-1, "unexpected PING reply");
    return false;
}

bool RedisConnection::isConnected() const
{
    return ctx_ != nullptr && connected_;
}

RedisReplyWrapper RedisConnection::command(const std::string& cmd)
{
    if (!isConnected())
    {
        setError(-1, "connection not established");
        return RedisReplyWrapper();
    }

    resetError();

    redisReply* reply = static_cast<redisReply*>(redisCommand(ctx_, cmd.c_str()));
    if (reply == nullptr)
    {
        setErrorFromContext();
        connected_ = false;
        return RedisReplyWrapper();
    }

    if (reply->type == REDIS_REPLY_ERROR)
    {
        setError(-1, reply->str ? reply->str : "redis command error");
    }

    return RedisReplyWrapper(reply);
}

bool RedisConnection::set(const std::string& key, const std::string& value)
{
    if (!isConnected())
    {
        setError(-1, "connection not established");
        return false;
    }

    resetError();

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "SET %b %b",
            key.data(), key.size(),
            value.data(), value.size()))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        connected_ = false;
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    return reply.isStatus() && reply.str() == "OK";
}

bool RedisConnection::get(const std::string& key, std::string* value)
{
    if (value == nullptr)
    {
        setError(-1, "value output pointer is null");
        return false;
    }

    if (!isConnected())
    {
        setError(-1, "connection not established");
        return false;
    }

    resetError();

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "GET %b", key.data(), key.size()))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        connected_ = false;
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    if (reply.isNil())
    {
        value->clear();
        return false;
    }

    if (!reply.isString())
    {
        setError(-1, "GET returned non-string reply");
        return false;
    }

    *value = reply.str();
    return true;
}

bool RedisConnection::del(const std::string& key, std::uint64_t* deleted_count)
{
    if (!isConnected())
    {
        setError(-1, "connection not established");
        return false;
    }

    resetError();

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "DEL %b", key.data(), key.size()))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        connected_ = false;
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    if (!reply.isInteger())
    {
        setError(-1, "DEL returned non-integer reply");
        return false;
    }

    if (deleted_count != nullptr)
    {
        *deleted_count = static_cast<std::uint64_t>(reply.integer());
    }

    return true;
}

bool RedisConnection::exists(const std::string& key, bool* exists_flag)
{
    if (exists_flag == nullptr)
    {
        setError(-1, "exists output pointer is null");
        return false;
    }

    if (!isConnected())
    {
        setError(-1, "connection not established");
        return false;
    }

    resetError();

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "EXISTS %b", key.data(), key.size()))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        connected_ = false;
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    if (!reply.isInteger())
    {
        setError(-1, "EXISTS returned non-integer reply");
        return false;
    }

    *exists_flag = (reply.integer() != 0);
    return true;
}

bool RedisConnection::expire(const std::string& key, int seconds)
{
    if (!isConnected())
    {
        setError(-1, "connection not established");
        return false;
    }

    resetError();

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "EXPIRE %b %d",
            key.data(), key.size(), seconds))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        connected_ = false;
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    if (!reply.isInteger())
    {
        setError(-1, "EXPIRE returned non-integer reply");
        return false;
    }

    return reply.integer() == 1;
}

bool RedisConnection::hset(const std::string& key, const std::string& field, const std::string& value)
{
    if (!isConnected())
    {
        setError(-1, "connection not established");
        return false;
    }

    resetError();

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "HSET %b %b %b",
            key.data(), key.size(),
            field.data(), field.size(),
            value.data(), value.size()))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        connected_ = false;
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    if (!reply.isInteger())
    {
        setError(-1, "HSET returned non-integer reply");
        return false;
    }

    return true;
}

bool RedisConnection::hget(const std::string& key, const std::string& field, std::string* value)
{
    if (value == nullptr)
    {
        setError(-1, "value output pointer is null");
        return false;
    }

    if (!isConnected())
    {
        setError(-1, "connection not established");
        return false;
    }

    resetError();

    RedisReplyWrapper reply(
        static_cast<redisReply*>(redisCommand(ctx_, "HGET %b %b",
            key.data(), key.size(),
            field.data(), field.size()))
    );

    if (!reply.valid())
    {
        setErrorFromContext();
        connected_ = false;
        return false;
    }

    if (reply.isError())
    {
        setError(-1, reply.str());
        return false;
    }

    if (reply.isNil())
    {
        value->clear();
        return false;
    }

    if (!reply.isString())
    {
        setError(-1, "HGET returned non-string reply");
        return false;
    }

    *value = reply.str();
    return true;
}

const std::string& RedisConnection::lastError() const
{
    return last_error_;
}

int RedisConnection::lastErrorCode() const
{
    return last_error_code_;
}

redisContext* RedisConnection::raw()
{
    return ctx_;
}