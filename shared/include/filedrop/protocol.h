// FileDrop — giao thức điều khiển qua UDP.
//
// Định dạng gói tin (UTF-8, dạng văn bản):
//   FILEDROP/1 <TYPE>\n
//   key=value\n
//   ...
// Giá trị được escape: '\\' -> "\\\\", '\n' -> "\\n", '\r' -> "\\r".
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace filedrop {

inline constexpr uint16_t kDiscoveryPort = 45454;
inline constexpr size_t kMaxDatagramSize = 8192;
inline constexpr const char* kProtocolMagic = "FILEDROP/1";

enum class MessageType {
    Discover,      // Hỏi danh tính (broadcast). Kèm danh tính của người hỏi.
    Announce,      // Trả lời danh tính: tên máy, IP, port UDP, hệ điều hành.
    Bye,           // Máy sắp tắt.
    SendRequest,   // Yêu cầu gửi file: tên file (1 file) hoặc số lượng file, tên máy gửi.
    SendResponse,  // Chấp nhận (kèm IP, port TCP, mật khẩu) hoặc từ chối.
    Cancel,        // Hủy một yêu cầu/phiên truyền.
};

const char* toString(MessageType type);
std::optional<MessageType> parseMessageType(std::string_view text);

// Tên các trường dùng trong gói tin.
namespace field {
inline constexpr const char* kId = "id";          // ID thiết bị (cố định theo máy)
inline constexpr const char* kName = "name";      // tên máy
inline constexpr const char* kOs = "os";          // macos | windows | linux | ios | android
inline constexpr const char* kIp = "ip";          // IP (UDP hoặc TCP tùy gói)
inline constexpr const char* kPort = "port";      // port UDP nhận gói điều khiển
inline constexpr const char* kRequest = "req";    // ID yêu cầu/phiên truyền
inline constexpr const char* kCount = "count";    // số lượng file
inline constexpr const char* kFile = "file";      // tên file (khi chỉ có 1 file)
inline constexpr const char* kSize = "size";      // tổng số byte
inline constexpr const char* kAccept = "accept";  // 1 = chấp nhận, 0 = từ chối
inline constexpr const char* kTcpPort = "tcp";    // port TCP server của máy nhận
inline constexpr const char* kPassword = "pass";  // mật khẩu 16 ký tự
}  // namespace field

class Message {
public:
    Message() = default;
    explicit Message(MessageType type) : type_(type) {}

    MessageType type() const { return type_; }

    void set(const std::string& key, const std::string& value) { fields_[key] = value; }
    void setInt(const std::string& key, int64_t value) { fields_[key] = std::to_string(value); }
    void setUInt(const std::string& key, uint64_t value) { fields_[key] = std::to_string(value); }

    bool has(const std::string& key) const { return fields_.count(key) != 0; }
    std::string get(const std::string& key, const std::string& fallback = {}) const;
    int64_t getInt(const std::string& key, int64_t fallback = 0) const;
    uint64_t getUInt(const std::string& key, uint64_t fallback = 0) const;

    std::string encode() const;
    static std::optional<Message> decode(std::string_view data);

private:
    MessageType type_ = MessageType::Discover;
    std::map<std::string, std::string> fields_;
};

}  // namespace filedrop
