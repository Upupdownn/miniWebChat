#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Protocol.hpp"

class Server;
class Connection;

class ChatService {
public:
    explicit ChatService(Server* server) : server_(server) {}

    void onMessage(const std::shared_ptr<Connection>& conn, const std::string& raw_json);
    void onConnectionClosed(const std::shared_ptr<Connection>& conn);

private:
    struct UserRecord {
        int id {0};
        std::string username;
        std::string password;
        bool online {false};
    };

    bool validateLoginRequest(const protocol::ClientMessage& msg, std::string& err) const;
    bool validateChatRequest(const protocol::ClientMessage& msg, std::string& err) const;

    bool loadUserFromRedis(const std::string& username, UserRecord& user);
    bool loadUserFromMySQL(const std::string& username, UserRecord& user);
    bool cacheUserToRedis(const UserRecord& user);
    bool setUserOnlineStatus(const std::string& username, bool online);
    bool cacheChatRecord(const std::shared_ptr<Connection>& conn, const std::string& text);

    std::string userCacheKey(const std::string& username) const {
        return "chat:user:" + username;
    }

    std::string chatCacheKey(const std::shared_ptr<Connection>& conn) const;

    static std::string sqlEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char ch : s) {
            if (ch == '\\' || ch == '\'' || ch == '"') {
                out += '\\';
            }
            out += ch;
        }
        return out;
    }

    static long long nowMillis() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    void sendError(const std::shared_ptr<Connection>& conn,
                   const std::string& request_id,
                   int code,
                   const std::string& message);

    void sendLoginResp(const std::shared_ptr<Connection>& conn,
                       const std::string& request_id,
                       bool success,
                       int code,
                       const std::string& message,
                       int user_id = 0,
                       const std::string& username = "");

    void broadcastChat(const std::shared_ptr<Connection>& sender, const std::string& text);
    void broadcastSystem(const std::string& message);

private:
    Server* server_ {nullptr};
    inline static std::atomic<std::uint64_t> chat_seq_ {0};
};

