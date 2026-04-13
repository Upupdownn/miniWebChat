#pragma once

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "Protocol.hpp"
#include "Reactor.hpp"

class Server;

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(int fd, Reactor* reactor, Server* server)
        : fd_(fd), reactor_(reactor), server_(server) {}

    ~Connection() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    int fd() const { return fd_; }
    bool isConnected() const { return connected_.load(); }
    bool isLoggedIn() const { return logged_in_.load(); }
    int userId() const { return user_id_; }
    const std::string& username() const { return username_; }
    const std::string& password() const { return password_; }

    void setUserInfo(int user_id, const std::string& username, const std::string& password) {
        user_id_ = user_id;
        username_ = username;
        password_ = password;
    }

    void setLoggedIn(bool value) { logged_in_.store(value); }

    bool registerToReactor();
    void handleRead();
    void handleWrite();
    void handleError();
    void closeAndCleanup();

    void sendJson(const std::string& json) {
        sendPacket(protocol::Protocol::frameMessage(json));
    }

    void sendPacket(const std::string& framed) {
        if (!isConnected()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(write_mtx_);
            write_buffer_ += framed;
        }
        if (reactor_ != nullptr) {
            reactor_->mod_fd(fd_, EPOLLIN | EPOLLOUT);
        }
    }

private:
    static bool setNonBlocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) return false;
        return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

private:
    int fd_ {-1};
    Reactor* reactor_ {nullptr};
    Server* server_ {nullptr};

    std::string read_buffer_;
    std::string write_buffer_;
    mutable std::mutex write_mtx_;

    std::atomic<bool> connected_ {true};
    std::atomic<bool> logged_in_ {false};

    int user_id_ {0};
    std::string username_;
    std::string password_;
};

