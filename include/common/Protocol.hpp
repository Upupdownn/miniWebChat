#pragma once

#include <arpa/inet.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace protocol {

struct ClientMessage {
    std::string type;
    std::string request_id;
    std::unordered_map<std::string, std::string> data;
};

class Protocol {
public:
    static constexpr std::uint32_t kHeaderLen = 4;
    static constexpr std::uint32_t kMaxBodyLen = 64 * 1024;

    static std::string frameMessage(const std::string& json) {
        std::string out;
        out.resize(kHeaderLen);
        const std::uint32_t len = static_cast<std::uint32_t>(json.size());
        const std::uint32_t nlen = htonl(len);
        std::memcpy(out.data(), &nlen, sizeof(nlen));
        out += json;
        return out;
    }

    static bool tryExtractFrame(std::string& buffer, std::string& json) {
        if (buffer.size() < kHeaderLen) {
            return false;
        }
        std::uint32_t nlen = 0;
        std::memcpy(&nlen, buffer.data(), sizeof(nlen));
        const std::uint32_t len = ntohl(nlen);
        if (len == 0 || len > kMaxBodyLen) {
            return false;
        }
        if (buffer.size() < kHeaderLen + len) {
            return false;
        }
        json.assign(buffer.data() + kHeaderLen, len);
        buffer.erase(0, kHeaderLen + len);
        return true;
    }

    static bool hasInvalidFrameLength(const std::string& buffer) {
        if (buffer.size() < kHeaderLen) {
            return false;
        }
        std::uint32_t nlen = 0;
        std::memcpy(&nlen, buffer.data(), sizeof(nlen));
        const std::uint32_t len = ntohl(nlen);
        return len == 0 || len > kMaxBodyLen;
    }

    static std::string escapeJson(const std::string& input) {
        std::string out;
        out.reserve(input.size() + 16);
        for (char ch : input) {
            switch (ch) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += ch; break;
            }
        }
        return out;
    }

    static std::string unescapeJson(const std::string& input) {
        std::string out;
        out.reserve(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '\\' && i + 1 < input.size()) {
                const char next = input[i + 1];
                switch (next) {
                    case 'n': out += '\n'; ++i; break;
                    case 'r': out += '\r'; ++i; break;
                    case 't': out += '\t'; ++i; break;
                    case '\\': out += '\\'; ++i; break;
                    case '"': out += '"'; ++i; break;
                    default: out += input[i]; break;
                }
            } else {
                out += input[i];
            }
        }
        return out;
    }

    static std::string buildResponse(
        const std::string& type,
        const std::string& request_id,
        const std::vector<std::pair<std::string, std::string>>& string_fields,
        const std::vector<std::pair<std::string, long long>>& int_fields = {},
        const std::vector<std::pair<std::string, bool>>& bool_fields = {}) {
        std::ostringstream oss;
        oss << "{\"type\":\"" << escapeJson(type) << "\","
            << "\"request_id\":\"" << escapeJson(request_id) << "\","
            << "\"data\":{";
        bool first = true;
        auto comma = [&]() {
            if (!first) oss << ',';
            first = false;
        };
        for (const auto& [k, v] : string_fields) {
            comma();
            oss << '\"' << escapeJson(k) << "\":\"" << escapeJson(v) << '\"';
        }
        for (const auto& [k, v] : int_fields) {
            comma();
            oss << '\"' << escapeJson(k) << "\":" << v;
        }
        for (const auto& [k, v] : bool_fields) {
            comma();
            oss << '\"' << escapeJson(k) << "\":" << (v ? "true" : "false");
        }
        oss << "}}";
        return oss.str();
    }

    static bool parseClientMessage(const std::string& json, ClientMessage& msg, std::string& err) {
        msg = ClientMessage{};
        if (!extractStringValue(json, "type", msg.type)) {
            err = "missing or invalid field: type";
            return false;
        }
        extractStringValue(json, "request_id", msg.request_id);

        std::string data_obj;
        if (!extractObjectValue(json, "data", data_obj)) {
            err = "missing or invalid field: data";
            return false;
        }

        if (msg.type == "login_req") {
            std::string username;
            std::string password;
            if (!extractStringValue(data_obj, "username", username)) {
                err = "missing or invalid field: data.username";
                return false;
            }
            if (!extractStringValue(data_obj, "password", password)) {
                err = "missing or invalid field: data.password";
                return false;
            }
            msg.data["username"] = username;
            msg.data["password"] = password;
            return true;
        }

        if (msg.type == "chat_req") {
            std::string text;
            if (!extractStringValue(data_obj, "text", text)) {
                err = "missing or invalid field: data.text";
                return false;
            }
            msg.data["text"] = text;
            return true;
        }

        err = "unsupported message type";
        return false;
    }

private:
    static void skipSpaces(const std::string& s, std::size_t& pos) {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
            ++pos;
        }
    }

    static bool extractStringValue(const std::string& src, const std::string& key, std::string& out) {
        const std::string token = "\"" + key + "\"";
        std::size_t pos = src.find(token);
        if (pos == std::string::npos) return false;
        pos = src.find(':', pos + token.size());
        if (pos == std::string::npos) return false;
        ++pos;
        skipSpaces(src, pos);
        if (pos >= src.size() || src[pos] != '"') return false;
        ++pos;
        std::string raw;
        bool escaped = false;
        for (; pos < src.size(); ++pos) {
            char ch = src[pos];
            if (escaped) {
                raw += '\\';
                raw += ch;
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                out = unescapeJson(raw);
                return true;
            }
            raw += ch;
        }
        return false;
    }

    static bool extractObjectValue(const std::string& src, const std::string& key, std::string& out) {
        const std::string token = "\"" + key + "\"";
        std::size_t pos = src.find(token);
        if (pos == std::string::npos) return false;
        pos = src.find(':', pos + token.size());
        if (pos == std::string::npos) return false;
        ++pos;
        skipSpaces(src, pos);
        if (pos >= src.size() || src[pos] != '{') return false;
        std::size_t start = pos;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (; pos < src.size(); ++pos) {
            char ch = src[pos];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
                continue;
            }
            if (ch == '"') {
                in_string = true;
                continue;
            }
            if (ch == '{') ++depth;
            if (ch == '}') {
                --depth;
                if (depth == 0) {
                    out = src.substr(start, pos - start + 1);
                    return true;
                }
            }
        }
        return false;
    }
};

}  // namespace protocol
