#include "MySQLConnection.h"

#include <utility>

MySQLResult::MySQLResult(MYSQL_RES* res)
    : res_(res)
{
}

MySQLResult::~MySQLResult()
{
    if (res_ != nullptr)
    {
        mysql_free_result(res_);
        res_ = nullptr;
    }
}

MySQLResult::MySQLResult(MySQLResult&& other) noexcept
    : res_(other.res_)
{
    other.res_ = nullptr;
}

MySQLResult& MySQLResult::operator=(MySQLResult&& other) noexcept
{
    if (this != &other)
    {
        if (res_ != nullptr)
        {
            mysql_free_result(res_);
        }
        res_ = other.res_;
        other.res_ = nullptr;
    }
    return *this;
}

bool MySQLResult::valid() const
{
    return res_ != nullptr;
}

MYSQL_RES* MySQLResult::get() const
{
    return res_;
}

MYSQL_ROW MySQLResult::fetchRow() const
{
    return res_ ? mysql_fetch_row(res_) : nullptr;
}

unsigned long* MySQLResult::fetchLengths() const
{
    return res_ ? mysql_fetch_lengths(res_) : nullptr;
}

unsigned int MySQLResult::fieldCount() const
{
    return res_ ? mysql_num_fields(res_) : 0;
}

my_ulonglong MySQLResult::rowCount() const
{
    return res_ ? mysql_num_rows(res_) : 0;
}

MySQLConnection::MySQLConnection() = default;

MySQLConnection::MySQLConnection(const MySQLConfig& config)
{
    connect(config);
}

MySQLConnection::~MySQLConnection()
{
    disconnect();
}

MySQLConnection::MySQLConnection(MySQLConnection&& other) noexcept
{
    moveFrom(other);
}

MySQLConnection& MySQLConnection::operator=(MySQLConnection&& other) noexcept
{
    if (this != &other)
    {
        disconnect();
        moveFrom(other);
    }
    return *this;
}

void MySQLConnection::moveFrom(MySQLConnection& other)
{
    conn_ = other.conn_;
    config_ = other.config_;
    connected_ = other.connected_;
    last_error_ = std::move(other.last_error_);
    last_error_code_ = other.last_error_code_;

    other.conn_ = nullptr;
    other.connected_ = false;
    other.last_error_code_ = 0;
    other.last_error_.clear();
}

void MySQLConnection::resetError()
{
    last_error_.clear();
    last_error_code_ = 0;
}

void MySQLConnection::setErrorFromMySQL()
{
    if (conn_ != nullptr)
    {
        last_error_ = mysql_error(conn_);
        last_error_code_ = mysql_errno(conn_);
    }
    else
    {
        last_error_ = "mysql connection is null";
        last_error_code_ = 0;
    }
}

bool MySQLConnection::applyOptions()
{
    if (conn_ == nullptr)
    {
        last_error_ = "mysql_init failed";
        return false;
    }

    unsigned int connect_timeout = static_cast<unsigned int>(config_.connect_timeout_sec);
    unsigned int read_timeout = static_cast<unsigned int>(config_.read_timeout_sec);
    unsigned int write_timeout = static_cast<unsigned int>(config_.write_timeout_sec);

    if (mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout) != 0)
    {
        setErrorFromMySQL();
        return false;
    }

    if (mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &read_timeout) != 0)
    {
        setErrorFromMySQL();
        return false;
    }

    if (mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout) != 0)
    {
        setErrorFromMySQL();
        return false;
    }

    if (!config_.charset.empty())
    {
        if (mysql_options(conn_, MYSQL_SET_CHARSET_NAME, config_.charset.c_str()) != 0)
        {
            setErrorFromMySQL();
            return false;
        }
    }

    return true;
}

bool MySQLConnection::connect(const MySQLConfig& config)
{
    disconnect();

    config_ = config;
    resetError();

    conn_ = mysql_init(nullptr);
    if (conn_ == nullptr)
    {
        last_error_ = "mysql_init failed";
        return false;
    }

    if (!applyOptions())
    {
        disconnect();
        return false;
    }

    MYSQL* ret = mysql_real_connect(
        conn_,
        config_.host.c_str(),
        config_.user.c_str(),
        config_.password.c_str(),
        config_.database.c_str(),
        config_.port,
        nullptr,
        0
    );

    if (ret == nullptr)
    {
        setErrorFromMySQL();
        disconnect();
        return false;
    }

    connected_ = true;
    return true;
}

void MySQLConnection::disconnect()
{
    if (conn_ != nullptr)
    {
        mysql_close(conn_);
        conn_ = nullptr;
    }

    connected_ = false;
}

bool MySQLConnection::reconnect()
{
    return connect(config_);
}

bool MySQLConnection::ping()
{
    if (!isConnected())
    {
        last_error_ = "connection not established";
        last_error_code_ = 0;
        return false;
    }

    if (mysql_ping(conn_) == 0)
    {
        return true;
    }

    setErrorFromMySQL();
    connected_ = false;
    return false;
}

bool MySQLConnection::isConnected() const
{
    return conn_ != nullptr && connected_;
}

bool MySQLConnection::executeInsert(const std::string& sql, std::uint64_t* insert_id)
{
    if (!isConnected())
    {
        last_error_ = "connection not established";
        last_error_code_ = 0;
        return false;
    }

    resetError();

    if (mysql_query(conn_, sql.c_str()) != 0)
    {
        setErrorFromMySQL();
        return false;
    }

    if (insert_id != nullptr)
    {
        *insert_id = static_cast<std::uint64_t>(mysql_insert_id(conn_));
    }

    return true;
}

bool MySQLConnection::executeUpdate(const std::string& sql, std::uint64_t* affected_rows)
{
    if (!isConnected())
    {
        last_error_ = "connection not established";
        last_error_code_ = 0;
        return false;
    }

    resetError();

    if (mysql_query(conn_, sql.c_str()) != 0)
    {
        setErrorFromMySQL();
        return false;
    }

    if (affected_rows != nullptr)
    {
        *affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(conn_));
    }

    return true;
}

bool MySQLConnection::executeDelete(const std::string& sql, std::uint64_t* affected_rows)
{
    return executeUpdate(sql, affected_rows);
}

bool MySQLConnection::executeModify(const std::string& sql, std::uint64_t* affected_rows)
{
    return executeUpdate(sql, affected_rows);
}

MySQLResult MySQLConnection::executeQuery(const std::string& sql)
{
    if (!isConnected())
    {
        last_error_ = "connection not established";
        last_error_code_ = 0;
        return MySQLResult();
    }

    resetError();

    if (mysql_query(conn_, sql.c_str()) != 0)
    {
        setErrorFromMySQL();
        return MySQLResult();
    }

    MYSQL_RES* res = mysql_store_result(conn_);
    if (res == nullptr)
    {
        if (mysql_field_count(conn_) == 0)
        {
            return MySQLResult();
        }

        setErrorFromMySQL();
        return MySQLResult();
    }

    return MySQLResult(res);
}

const std::string& MySQLConnection::lastError() const
{
    return last_error_;
}

unsigned int MySQLConnection::lastErrorCode() const
{
    return last_error_code_;
}

MYSQL* MySQLConnection::raw()
{
    return conn_;
}