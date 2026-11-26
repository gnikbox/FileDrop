// FileDrop — lớp bọc socket đa nền tảng (POSIX / Winsock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace filedrop::net {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

// Khởi tạo thư viện mạng (WSAStartup trên Windows). Gọi nhiều lần không sao.
void initialize();

// Socket RAII, chỉ di chuyển (move-only).
class Socket {
public:
    Socket() = default;
    explicit Socket(socket_t fd) : fd_(fd) {}
    ~Socket() { close(); }

    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidSocket; }
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return fd_ != kInvalidSocket; }
    socket_t fd() const { return fd_; }
    void close();

private:
    socket_t fd_ = kInvalidSocket;
};

struct Endpoint {
    std::string ip;
    uint16_t port = 0;
};

std::string lastErrorString();

// --- UDP ---
// Mở socket UDP IPv4, bind 0.0.0.0:port (port = 0 => port ngẫu nhiên), bật broadcast.
// reuse = true cho phép nhiều tiến trình cùng nghe port discovery.
Socket openUdp(uint16_t port, bool reuse, std::string* error);
bool sendTo(const Socket& sock, const std::string& ip, uint16_t port, const void* data, size_t len);
int recvFrom(const Socket& sock, void* buf, size_t len, Endpoint* from);
uint16_t localPort(const Socket& sock);

// Các địa chỉ broadcast: 255.255.255.255 + broadcast của từng card mạng IPv4 đang hoạt động.
std::vector<std::string> broadcastAddresses();
// IP cục bộ dùng để đi tới remoteIp (không gửi gói nào ra mạng).
std::string localIpFor(const std::string& remoteIp);
// IP cục bộ chính trong mạng LAN.
std::string primaryLocalIp();
// IP của card mạng LAN thật (Wi-Fi/Ethernet), bỏ qua VPN, máy ảo, Docker... — dùng để hiện địa chỉ cho người dùng.
std::string lanIp();

// --- TCP ---
Socket listenTcp(uint16_t port, std::string* error, int backlog = 4);
// Chờ tối đa timeoutMs; trả về socket không hợp lệ nếu hết giờ.
Socket acceptTcp(const Socket& listener, int timeoutMs, Endpoint* peer);
Socket connectTcp(const std::string& ip, uint16_t port, int timeoutMs, std::string* error);
void setIoTimeout(const Socket& sock, int timeoutMs);
bool sendAll(const Socket& sock, const void* data, size_t len);
bool recvAll(const Socket& sock, void* data, size_t len);
// Nhận tối đa len byte (ít nhất 1). Trả về số byte, 0 nếu bên kia đóng, <0 nếu lỗi/hết giờ.
int recvSome(const Socket& sock, void* data, size_t len);
// Ngắt cả 2 chiều — làm các lời gọi recv/send đang chờ trên socket này ở luồng khác trả về ngay.
void shutdownBoth(socket_t fd);

// select() trên nhiều socket. Trả về >0 nếu có socket đọc được, 0 nếu hết giờ, <0 nếu lỗi.
int waitReadable(const std::vector<socket_t>& fds, int timeoutMs, std::vector<bool>* ready);

}  // namespace filedrop::net
