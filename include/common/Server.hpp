#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ChatService.hpp"
#include "Connection.hpp"
#include "MySQLConnectionPool.h"
#include "Reactor.hpp"
#include "RedisConnectionPool.h"
#include "ThreadPool.hpp"

struct ServerConfig {
    std::string listen_ip {"0.0.0.0"};
    std::uint16_t listen_port {9000};
    int backlog {128};
    bool use_et {true};

    std::size_t thread_pool_core {4};
    std::size_t thread_pool_max {8};

    MySQLPoolConfig mysql_pool_config;
    RedisPoolConfig redis_pool_config;
};

class Server {
public:
    explicit Server(const ServerConfig& config)
        : config_(config),
          reactor_(config.use_et),
          thread_pool_(config.thread_pool_core, config.thread_pool_max),
          mysql_pool_(config.mysql_pool_config),
          redis_pool_(config.redis_pool_config),
          chat_service_(this) {}

    ~Server() { stop(); }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool init();
    void start() { reactor_.loop(1000); }
    void stop();

    Reactor& reactor() { return reactor_; }
    ThreadPool& threadPool() { return thread_pool_; }
    MySQLConnectionPool& mysqlPool() { return mysql_pool_; }
    RedisConnectionPool& redisPool() { return redis_pool_; }
    ChatService& chatService() { return chat_service_; }

    void removeConnection(int fd);
    std::vector<std::shared_ptr<Connection>> snapshotConnections();

private:
    bool initListenSocket();
    void handleAccept();

private:
    ServerConfig config_;
    int listen_fd_ {-1};
    bool stopped_ {false};

    Reactor reactor_;
    ThreadPool thread_pool_;
    MySQLConnectionPool mysql_pool_;
    RedisConnectionPool redis_pool_;
    ChatService chat_service_;

    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::mutex connections_mtx_;
};

// ======================== Connection impl ========================
inline bool Connection::registerToReactor() {
    if (fd_ < 0 || reactor_ == nullptr) {
        return false;
    }
    if (!setNonBlocking(fd_)) {
        return false;
    }
    Reactor::EventHandler handler;
    std::weak_ptr<Connection> weak_self = shared_from_this();
    handler.read_callback = [weak_self]() {
        if (auto self = weak_self.lock()) self->handleRead();
    };
    handler.write_callback = [weak_self]() {
        if (auto self = weak_self.lock()) self->handleWrite();
    };
    handler.error_callback = [weak_self]() {
        if (auto self = weak_self.lock()) self->handleError();
    };
    return reactor_->add_fd(fd_, EPOLLIN, handler);
}

inline void Connection::handleRead() {
    if (!isConnected()) {
        return;
    }
    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            read_buffer_.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            closeAndCleanup();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        closeAndCleanup();
        return;
    }

    if (protocol::Protocol::hasInvalidFrameLength(read_buffer_)) {
        closeAndCleanup();
        return;
    }

    std::string json;
    while (protocol::Protocol::tryExtractFrame(read_buffer_, json)) {
        auto self = shared_from_this();
        server_->threadPool().submit([this, self, json]() {
            server_->chatService().onMessage(self, json);
        });
    }
}

inline void Connection::handleWrite() {
    if (!isConnected()) {
        return;
    }
    std::lock_guard<std::mutex> lock(write_mtx_);
    while (!write_buffer_.empty()) {
        const ssize_t n = ::send(fd_, write_buffer_.data(), write_buffer_.size(), 0);
        if (n > 0) {
            write_buffer_.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        closeAndCleanup();
        return;
    }
    if (reactor_ != nullptr) {
        reactor_->mod_fd(fd_, EPOLLIN);
    }
}

inline void Connection::handleError() { closeAndCleanup(); }

inline void Connection::closeAndCleanup() {
    bool expected = true;
    if (!connected_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (reactor_ != nullptr) {
        reactor_->del_fd(fd_);
    }
    if (server_ != nullptr) {
        server_->chatService().onConnectionClosed(shared_from_this());
        server_->removeConnection(fd_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ======================== ChatService impl ========================
inline void ChatService::onMessage(const std::shared_ptr<Connection>& conn, const std::string& raw_json) {
    if (!conn || !conn->isConnected()) {
        return;
    }

    protocol::ClientMessage msg;
    std::string err;
    if (!protocol::Protocol::parseClientMessage(raw_json, msg, err)) {
        const int code = (err == "unsupported message type") ? 3003 : 3004;
        sendError(conn, msg.request_id, code, err);
        return;
    }

    if (msg.type == "login_req") {
        if (!validateLoginRequest(msg, err)) {
            sendLoginResp(conn, msg.request_id, false, 3004, err);
            return;
        }

        UserRecord user;
        if (!loadUserFromRedis(msg.data.at("username"), user)) {
            if (!loadUserFromMySQL(msg.data.at("username"), user)) {
                sendLoginResp(conn, msg.request_id, false, 1001, "invalid username or password");
                return;
            }
            cacheUserToRedis(user);
        }

        if (user.password != msg.data.at("password")) {
            sendLoginResp(conn, msg.request_id, false, 1001, "invalid username or password");
            return;
        }
        if (user.online) {
            sendLoginResp(conn, msg.request_id, false, 1002, "user already online");
            return;
        }

        conn->setUserInfo(user.id, user.username, user.password);
        conn->setLoggedIn(true);
        setUserOnlineStatus(user.username, true);
        sendLoginResp(conn, msg.request_id, true, 0, "login success", user.id, user.username);
        broadcastSystem(user.username + " joined the chat");
        return;
    }

    if (msg.type == "chat_req") {
        if (!conn->isLoggedIn()) {
            sendError(conn, msg.request_id, 3002, "user not logged in");
            return;
        }
        if (!validateChatRequest(msg, err)) {
            sendError(conn, msg.request_id, 3004, err);
            return;
        }
        cacheChatRecord(conn, msg.data.at("text"));
        broadcastChat(conn, msg.data.at("text"));
        return;
    }
}

inline void ChatService::onConnectionClosed(const std::shared_ptr<Connection>& conn) {
    if (conn && conn->isLoggedIn()) {
        setUserOnlineStatus(conn->username(), false);
        broadcastSystem(conn->username() + " left the chat");
        conn->setLoggedIn(false);
    }
}

inline bool ChatService::validateLoginRequest(const protocol::ClientMessage& msg, std::string& err) const {
    const auto it_user = msg.data.find("username");
    const auto it_pass = msg.data.find("password");
    if (it_user == msg.data.end() || it_user->second.empty()) {
        err = "username cannot be empty";
        return false;
    }
    if (it_pass == msg.data.end() || it_pass->second.empty()) {
        err = "password cannot be empty";
        return false;
    }
    if (it_user->second.size() > 32) {
        err = "username too long";
        return false;
    }
    if (it_pass->second.size() > 64) {
        err = "password too long";
        return false;
    }
    return true;
}

inline bool ChatService::validateChatRequest(const protocol::ClientMessage& msg, std::string& err) const {
    const auto it = msg.data.find("text");
    if (it == msg.data.end() || it->second.empty()) {
        err = "chat text cannot be empty";
        return false;
    }
    if (it->second.size() > 1024) {
        err = "chat text too long";
        return false;
    }
    return true;
}

inline bool ChatService::loadUserFromRedis(const std::string& username, UserRecord& user) {
    auto conn = server_->redisPool().acquire();
    if (!conn.valid()) return false;

    const std::string key = userCacheKey(username);
    std::string id_str, uname, passwd, online_str;
    if (!conn->hget(key, "user_id", &id_str)) return false;
    if (!conn->hget(key, "username", &uname)) return false;
    if (!conn->hget(key, "password", &passwd)) return false;
    conn->hget(key, "online", &online_str);

    try {
        user.id = std::stoi(id_str);
    } catch (...) {
        return false;
    }
    user.username = uname;
    user.password = passwd;
    user.online = (online_str == "1");
    return true;
}

inline bool ChatService::loadUserFromMySQL(const std::string& username, UserRecord& user) {
    auto conn = server_->mysqlPool().acquire();
    if (!conn.valid()) return false;

    const std::string sql =
        "SELECT id, username, password FROM users WHERE username='" + sqlEscape(username) + "' LIMIT 1";

    auto result = conn->executeQuery(sql);
    if (!result.valid()) {
        return false;
    }
    MYSQL_ROW row = result.fetchRow();
    if (row == nullptr) {
        return false;
    }
    user.id = row[0] ? std::atoi(row[0]) : 0;
    user.username = row[1] ? row[1] : "";
    user.password = row[2] ? row[2] : "";
    user.online = false;
    return user.id > 0 && !user.username.empty();
}

inline bool ChatService::cacheUserToRedis(const UserRecord& user) {
    auto conn = server_->redisPool().acquire();
    if (!conn.valid()) return false;
    const std::string key = userCacheKey(user.username);
    bool ok = true;
    ok = ok && conn->hset(key, "user_id", std::to_string(user.id));
    ok = ok && conn->hset(key, "username", user.username);
    ok = ok && conn->hset(key, "password", user.password);
    ok = ok && conn->hset(key, "online", user.online ? "1" : "0");
    conn->expire(key, 24 * 3600);
    return ok;
}

inline bool ChatService::setUserOnlineStatus(const std::string& username, bool online) {
    auto conn = server_->redisPool().acquire();
    if (!conn.valid()) return false;
    const std::string key = userCacheKey(username);
    const bool ok = conn->hset(key, "online", online ? "1" : "0");
    conn->expire(key, 24 * 3600);
    return ok;
}

inline std::string ChatService::chatCacheKey(const std::shared_ptr<Connection>& conn) const {
    std::ostringstream oss;
    oss << "chat:world:" << nowMillis() << ':' << conn->userId() << ':' << chat_seq_.fetch_add(1);
    return oss.str();
}

inline bool ChatService::cacheChatRecord(const std::shared_ptr<Connection>& conn, const std::string& text) {
    auto redis = server_->redisPool().acquire();
    if (!redis.valid()) return false;
    const std::string json = protocol::Protocol::buildResponse(
        "chat_record",
        "",
        {{"username", conn->username()}, {"text", text}},
        {{"user_id", conn->userId()}, {"timestamp", nowMillis()}},
        {}
    );
    const std::string key = chatCacheKey(conn);
    const bool ok = redis->set(key, json);
    if (ok) {
        redis->expire(key, 24 * 3600);
    }
    return ok;
}

inline void ChatService::sendError(const std::shared_ptr<Connection>& conn,
                                   const std::string& request_id,
                                   int code,
                                   const std::string& message) {
    conn->sendJson(protocol::Protocol::buildResponse(
        "error", request_id, {{"message", message}}, {{"code", code}}, {}));
}

inline void ChatService::sendLoginResp(const std::shared_ptr<Connection>& conn,
                                       const std::string& request_id,
                                       bool success,
                                       int code,
                                       const std::string& message,
                                       int user_id,
                                       const std::string& username) {
    std::vector<std::pair<std::string, std::string>> strings{{"message", message}};
    std::vector<std::pair<std::string, long long>> ints{{"code", code}};
    std::vector<std::pair<std::string, bool>> bools{{"success", success}};
    if (success) {
        strings.push_back({"username", username});
        ints.push_back({"user_id", user_id});
    }
    conn->sendJson(protocol::Protocol::buildResponse("login_resp", request_id, strings, ints, bools));
}

inline void ChatService::broadcastChat(const std::shared_ptr<Connection>& sender, const std::string& text) {
    const std::string payload = protocol::Protocol::buildResponse(
        "chat_broadcast", "", {{"username", sender->username()}, {"text", text}}, {{"user_id", sender->userId()}}, {});
    for (const auto& conn : server_->snapshotConnections()) {
        if (conn && conn->isConnected() && conn->isLoggedIn()) {
            conn->sendJson(payload);
        }
    }
}

inline void ChatService::broadcastSystem(const std::string& message) {
    const std::string payload = protocol::Protocol::buildResponse(
        "system", "", {{"message", message}}, {{"code", 2001}}, {});
    for (const auto& conn : server_->snapshotConnections()) {
        if (conn && conn->isConnected() && conn->isLoggedIn()) {
            conn->sendJson(payload);
        }
    }
}

// ======================== Server impl ========================
inline bool Server::initListenSocket() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::perror("socket");
        return false;
    }

    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    int flags = ::fcntl(listen_fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.listen_port);
    if (::inet_pton(AF_INET, config_.listen_ip.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "invalid listen ip: " << config_.listen_ip << std::endl;
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return false;
    }
    if (::listen(listen_fd_, config_.backlog) < 0) {
        std::perror("listen");
        return false;
    }
    return true;
}

inline bool Server::init() {
    if (!reactor_.init()) {
        std::cerr << "reactor init failed\n";
        return false;
    }
    if (!mysql_pool_.init()) {
        std::cerr << "mysql pool init failed\n";
        return false;
    }
    if (!redis_pool_.init()) {
        std::cerr << "redis pool init failed\n";
        return false;
    }
    if (!initListenSocket()) {
        return false;
    }

    Reactor::EventHandler handler;
    handler.read_callback = [this]() { handleAccept(); };
    handler.error_callback = [this]() { stop(); };

    if (!reactor_.add_fd(listen_fd_, EPOLLIN, handler)) {
        std::cerr << "failed to add listen fd to reactor\n";
        return false;
    }
    return true;
}

inline void Server::stop() {
    if (stopped_) return;
    stopped_ = true;
    reactor_.stop();

    std::vector<std::shared_ptr<Connection>> conns;
    {
        std::lock_guard<std::mutex> lock(connections_mtx_);
        for (auto& [fd, conn] : connections_) {
            (void)fd;
            conns.push_back(conn);
        }
    }
    for (auto& conn : conns) {
        if (conn) {
            conn->closeAndCleanup();
        }
    }
    {
        std::lock_guard<std::mutex> lock(connections_mtx_);
        connections_.clear();
    }
    if (listen_fd_ >= 0) {
        reactor_.del_fd(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    thread_pool_.shutdown();
}

inline void Server::handleAccept() {
    while (true) {
        sockaddr_in cli_addr{};
        socklen_t cli_len = sizeof(cli_addr);
        const int conn_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli_addr), &cli_len);
        if (conn_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            std::perror("accept");
            return;
        }

        auto conn = std::make_shared<Connection>(conn_fd, &reactor_, this);
        if (!conn->registerToReactor()) {
            ::close(conn_fd);
            continue;
        }
        std::lock_guard<std::mutex> lock(connections_mtx_);
        connections_[conn_fd] = conn;
    }
}

inline void Server::removeConnection(int fd) {
    std::lock_guard<std::mutex> lock(connections_mtx_);
    connections_.erase(fd);
}

inline std::vector<std::shared_ptr<Connection>> Server::snapshotConnections() {
    std::vector<std::shared_ptr<Connection>> out;
    std::lock_guard<std::mutex> lock(connections_mtx_);
    out.reserve(connections_.size());
    for (auto& [fd, conn] : connections_) {
        (void)fd;
        out.push_back(conn);
    }
    return out;
}

