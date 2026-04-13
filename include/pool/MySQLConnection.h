#pragma once

#include <mysql/mysql.h>

#include <cstdint>
#include <string>

struct MySQLConfig
{
    std::string host = "127.0.0.1";
    uint16_t    port = 3306;
    std::string user = "root";
    std::string password = "123456";
    std::string database = "test";
    std::string charset = "utf8mb4";

    int connect_timeout_sec = 3;
    int read_timeout_sec = 3;
    int write_timeout_sec = 3;
};

class MySQLResult
{
public:
    MySQLResult() = default;
    explicit MySQLResult(MYSQL_RES* res);
    ~MySQLResult();

    MySQLResult(const MySQLResult&) = delete;
    MySQLResult& operator=(const MySQLResult&) = delete;

    MySQLResult(MySQLResult&& other) noexcept;
    MySQLResult& operator=(MySQLResult&& other) noexcept;

    bool valid() const;
    MYSQL_RES* get() const;
    MYSQL_ROW fetchRow() const;
    unsigned long* fetchLengths() const;
    unsigned int fieldCount() const;
    my_ulonglong rowCount() const;

private:
    MYSQL_RES* res_ = nullptr;
};

class MySQLConnection
{
public:
    MySQLConnection();
    explicit MySQLConnection(const MySQLConfig& config);
    ~MySQLConnection();

    MySQLConnection(const MySQLConnection&) = delete;
    MySQLConnection& operator=(const MySQLConnection&) = delete;

    MySQLConnection(MySQLConnection&& other) noexcept;
    MySQLConnection& operator=(MySQLConnection&& other) noexcept;

public:
    bool connect(const MySQLConfig& config);
    void disconnect();
    bool reconnect();
    bool ping();
    bool isConnected() const;

    bool executeInsert(const std::string& sql, std::uint64_t* insert_id = nullptr);
    bool executeUpdate(const std::string& sql, std::uint64_t* affected_rows = nullptr);
    bool executeDelete(const std::string& sql, std::uint64_t* affected_rows = nullptr);
    bool executeModify(const std::string& sql, std::uint64_t* affected_rows = nullptr);
    MySQLResult executeQuery(const std::string& sql);

    const std::string& lastError() const;
    unsigned int lastErrorCode() const;

    MYSQL* raw();

private:
    bool applyOptions();
    void resetError();
    void setErrorFromMySQL();
    void moveFrom(MySQLConnection& other);

private:
    MYSQL* conn_ = nullptr;
    MySQLConfig config_;
    bool connected_ = false;

    std::string last_error_;
    unsigned int last_error_code_ = 0;
};