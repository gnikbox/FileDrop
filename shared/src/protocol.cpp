#include "filedrop/protocol.h"

#include <cstdlib>

namespace filedrop {

namespace {

struct TypeName {
    MessageType type;
    const char* name;
};

constexpr TypeName kTypeNames[] = {
    {MessageType::Discover, "DISCOVER"},
    {MessageType::Announce, "ANNOUNCE"},
    {MessageType::Bye, "BYE"},
    {MessageType::SendRequest, "SEND_REQUEST"},
    {MessageType::SendResponse, "SEND_RESPONSE"},
    {MessageType::Cancel, "CANCEL"},
};

std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string unescape(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '\\' && i + 1 < in.size()) {
            char n = in[++i];
            if (n == 'n') out.push_back('\n');
            else if (n == 'r') out.push_back('\r');
            else out.push_back(n);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

const char* toString(MessageType type) {
    for (const auto& t : kTypeNames) {
        if (t.type == type) return t.name;
    }
    return "UNKNOWN";
}

std::optional<MessageType> parseMessageType(std::string_view text) {
    for (const auto& t : kTypeNames) {
        if (text == t.name) return t.type;
    }
    return std::nullopt;
}

std::string Message::get(const std::string& key, const std::string& fallback) const {
    auto it = fields_.find(key);
    return it == fields_.end() ? fallback : it->second;
}

int64_t Message::getInt(const std::string& key, int64_t fallback) const {
    auto it = fields_.find(key);
    if (it == fields_.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    long long v = std::strtoll(it->second.c_str(), &end, 10);
    return (end && *end == '\0') ? static_cast<int64_t>(v) : fallback;
}

uint64_t Message::getUInt(const std::string& key, uint64_t fallback) const {
    auto it = fields_.find(key);
    if (it == fields_.end() || it->second.empty() || it->second[0] == '-') return fallback;
    char* end = nullptr;
    unsigned long long v = std::strtoull(it->second.c_str(), &end, 10);
    return (end && *end == '\0') ? static_cast<uint64_t>(v) : fallback;
}

std::string Message::encode() const {
    std::string out = kProtocolMagic;
    out += ' ';
    out += toString(type_);
    out += '\n';
    for (const auto& [key, value] : fields_) {
        out += key;
        out += '=';
        out += escape(value);
        out += '\n';
    }
    return out;
}

std::optional<Message> Message::decode(std::string_view data) {
    if (data.size() > kMaxDatagramSize) return std::nullopt;

    size_t pos = data.find('\n');
    std::string_view header = data.substr(0, pos);
    if (!header.empty() && header.back() == '\r') header.remove_suffix(1);

    const std::string_view magic = kProtocolMagic;
    if (header.size() <= magic.size() + 1 || header.substr(0, magic.size()) != magic ||
        header[magic.size()] != ' ') {
        return std::nullopt;
    }
    auto type = parseMessageType(header.substr(magic.size() + 1));
    if (!type) return std::nullopt;

    Message msg(*type);
    while (pos != std::string_view::npos) {
        size_t start = pos + 1;
        pos = data.find('\n', start);
        std::string_view line =
            data.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        msg.fields_[std::string(line.substr(0, eq))] = unescape(line.substr(eq + 1));
    }
    return msg;
}

}  // namespace filedrop
