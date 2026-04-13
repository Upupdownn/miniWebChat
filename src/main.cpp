#include <csignal>
#include <cstdlib>
#include <iostream>

#include "Server.hpp"

namespace {
Server* g_server = nullptr;

void onSignal(int) 
{
    if (g_server != nullptr)
        g_server->stop();
}

// 从环境变量读取配置（有默认值）
std::string envOrDefault(const char* key, const char* def) 
{
    const char* v = std::getenv(key);
    return v ? v : def;
}

int envIntOrDefault(const char* key, int def) 
{
    const char* v = std::getenv(key);
    if (!v) return def;
    try {
        return std::stoi(v);
    } catch (...) {
        return def;
    }
}
}  // namespace

int main() 
{
    ServerConfig config;
    config.listen_ip = envOrDefault("CHAT_LISTEN_IP", "127.0.0.1");
    config.listen_port = static_cast<std::uint16_t>(envIntOrDefault("CHAT_LISTEN_PORT", 9000));
    config.thread_pool_core = static_cast<std::size_t>(envIntOrDefault("CHAT_THREAD_CORE", 4));
    config.thread_pool_max = static_cast<std::size_t>(envIntOrDefault("CHAT_THREAD_MAX", 8));

    config.mysql_pool_config.db_config.host = envOrDefault("CHAT_MYSQL_HOST", "127.0.0.1");
    config.mysql_pool_config.db_config.port = static_cast<std::uint16_t>(envIntOrDefault("CHAT_MYSQL_PORT", 3306));
    config.mysql_pool_config.db_config.user = envOrDefault("CHAT_MYSQL_USER", "szf517");
    config.mysql_pool_config.db_config.password = envOrDefault("CHAT_MYSQL_PASSWORD", "13047104099");
    config.mysql_pool_config.db_config.database = envOrDefault("CHAT_MYSQL_DB", "miniWebChat");
    config.mysql_pool_config.min_connections = static_cast<std::size_t>(envIntOrDefault("CHAT_MYSQL_MIN_CONN", 2));
    config.mysql_pool_config.max_connections = static_cast<std::size_t>(envIntOrDefault("CHAT_MYSQL_MAX_CONN", 8));

    config.redis_pool_config.redis_config.host = envOrDefault("CHAT_REDIS_HOST", "127.0.0.1");
    config.redis_pool_config.redis_config.port = static_cast<std::uint16_t>(envIntOrDefault("CHAT_REDIS_PORT", 6379));
    config.redis_pool_config.redis_config.password = envOrDefault("CHAT_REDIS_PASSWORD", "");
    config.redis_pool_config.redis_config.db = envIntOrDefault("CHAT_REDIS_DB", 0);
    config.redis_pool_config.min_connections = static_cast<std::size_t>(envIntOrDefault("CHAT_REDIS_MIN_CONN", 2));
    config.redis_pool_config.max_connections = static_cast<std::size_t>(envIntOrDefault("CHAT_REDIS_MAX_CONN", 8));

    Server server(config);
    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!server.init()) 
    {
        std::cerr << "server init failed" << std::endl;
        return 1;
    }

    std::cout << "chat server listening on " << config.listen_ip << ':' << config.listen_port << std::endl;
    server.start();
    std::cout << "chat server stopped" << std::endl;

    return 0;
}
