#include "filedrop/net.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace filedrop::net {

namespace {

#ifdef _WIN32
int closeSocket(socket_t s) { return ::closesocket(s); }
int lastErrorCode() { return ::WSAGetLastError(); }
bool wouldBlock(int code) { return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS; }
#else
int closeSocket(socket_t s) { return ::close(s); }
int lastErrorCode() { return errno; }
bool wouldBlock(int code) { return code == EINPROGRESS || code == EWOULDBLOCK; }
#endif

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

constexpr int kSocketBufferSize = 1 << 20;

void configureCommon([[maybe_unused]] socket_t s) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#ifndef _WIN32
    ::fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
}

bool toSockaddr(const std::string& ip, uint16_t port, sockaddr_in* out) {
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    return ::inet_pton(AF_INET, ip.c_str(), &out->sin_addr) == 1;
}

std::string ipToString(const in_addr& addr) {
    char buf[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, const_cast<in_addr*>(&addr), buf, sizeof(buf));
    return buf;
}

bool setNonBlocking(socket_t s, bool enable) {
#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(s, F_SETFL, flags) == 0;
#endif
}

void setBuffers(socket_t s) {
    int size = kSocketBufferSize;
    ::setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
}

void pushUnique(std::vector<std::string>& list, const std::string& value) {
    if (!value.empty() && std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

struct InterfaceAddr {
    in_addr addr;
    in_addr broadcast;
    std::string name;      // tên card (POSIX: en0...; Windows: mô tả adapter)
    bool wireless = false;  // Windows: IF_TYPE_IEEE80211
};

std::vector<InterfaceAddr> activeInterfaces() {
    std::vector<InterfaceAddr> result;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG ret = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && ret == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        ret = ::GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (ret != NO_ERROR) return result;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            uint32_t prefix = u->OnLinkPrefixLength;
            if (prefix == 0 || prefix > 32) continue;
            uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix);
            InterfaceAddr ia{};
            ia.addr = sin->sin_addr;
            ia.broadcast.s_addr = sin->sin_addr.s_addr | htonl(~mask);
            if (a->Description) {
                int n = ::WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    ia.name.resize(static_cast<size_t>(n - 1));
                    ::WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, ia.name.data(), n, nullptr, nullptr);
                }
            }
            ia.wireless = a->IfType == IF_TYPE_IEEE80211;
            result.push_back(ia);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return result;
    for (ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || !ifa->ifa_netmask) continue;
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_RUNNING)) continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_BROADCAST)) continue;
        auto* addr = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        auto* mask = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
        InterfaceAddr ia{};
        ia.addr = addr->sin_addr;
        ia.broadcast.s_addr = addr->sin_addr.s_addr | ~mask->sin_addr.s_addr;
        ia.name = ifa->ifa_name ? ifa->ifa_name : "";
        result.push_back(ia);
    }
    ::freeifaddrs(list);
#endif
    return result;
}

}  // namespace

void initialize() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = kInvalidSocket;
    }
    return *this;
}

void Socket::close() {
    if (fd_ != kInvalidSocket) {
        closeSocket(fd_);
        fd_ = kInvalidSocket;
    }
}

std::string lastErrorString() {
    int code = lastErrorCode();
#ifdef _WIN32
    return "WSA error " + std::to_string(code);
#else
    return std::string(std::strerror(code)) + " (" + std::to_string(code) + ")";
#endif
}

Socket openUdp(uint16_t port, bool reuse, std::string* error) {
    initialize();
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) {
        if (error) *error = "socket(UDP): " + lastErrorString();
        return {};
    }
    Socket sock(s);
    configureCommon(s);

    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
    if (reuse) {
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_REUSEPORT
        ::setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
    }
#ifdef _WIN32
    // Tránh WSAECONNRESET khi nhận ICMP "port unreachable".
    BOOL newBehavior = FALSE;
    DWORD bytes = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior), nullptr, 0, &bytes, nullptr, nullptr);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "bind(UDP " + std::to_string(port) + "): " + lastErrorString();
        return {};
    }
    return sock;
}

bool sendTo(const Socket& sock, const std::string& ip, uint16_t port, const void* data, size_t len) {
    if (!sock.valid()) return false;
    sockaddr_in addr{};
    if (!toSockaddr(ip, port, &addr)) return false;
    auto sent = ::sendto(sock.fd(), static_cast<const char*>(data), static_cast<int>(len), 0,
                         reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<decltype(sent)>(len);
}

int recvFrom(const Socket& sock, void* buf, size_t len, Endpoint* from) {
    sockaddr_in addr{};
    socklen_t addrLen = sizeof(addr);
    auto n = ::recvfrom(sock.fd(), static_cast<char*>(buf), static_cast<int>(len), 0,
                        reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (n >= 0 && from) {
        from->ip = ipToString(addr.sin_addr);
        from->port = ntohs(addr.sin_port);
    }
    return static_cast<int>(n);
}

uint16_t localPort(const Socket& sock) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(sock.fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

std::vector<std::string> broadcastAddresses() {
    std::vector<std::string> out;
    for (const auto& ia : activeInterfaces()) pushUnique(out, ipToString(ia.broadcast));
    pushUnique(out, "255.255.255.255");
    return out;
}

std::string localIpFor(const std::string& remoteIp) {
    initialize();
    sockaddr_in remote{};
    if (!toSockaddr(remoteIp, 9, &remote)) return {};
    Socket s(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!s.valid()) return {};
    if (::connect(s.fd(), reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) return {};
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (::getsockname(s.fd(), reinterpret_cast<sockaddr*>(&local), &len) != 0) return {};
    std::string ip = ipToString(local.sin_addr);
    return ip == "0.0.0.0" ? std::string() : ip;
}

std::string primaryLocalIp() {
    std::string ip = localIpFor("8.8.8.8");
    if (!ip.empty()) return ip;
    auto ifs = activeInterfaces();
    return ifs.empty() ? std::string("127.0.0.1") : ipToString(ifs.front().addr);
}

std::string lanIp() {
    // Chấm điểm từng card: ưu tiên card thật (Wi-Fi/Ethernet) hơn card ảo của VPN, máy ảo, Docker...
    static const char* kVirtual[] = {"bridge", "vmnet", "vboxnet", "docker", "veth", "utun", "tun", "tap", "awdl",
                                     "llw", "virtual", "vmware", "hyper-v", "vethernet", "wireguard", "vpn",
                                     "tailscale", "zerotier", "parallels", "loopback", "bluetooth"};
    const std::string routed = localIpFor("8.8.8.8");
    std::string best;
    int bestScore = -1;
    for (const auto& ia : activeInterfaces()) {
        std::string lower;
        for (char c : ia.name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        bool isVirtual = false;
        for (const char* v : kVirtual) isVirtual |= lower.find(v) != std::string::npos;
        const uint32_t ip = ntohl(ia.addr.s_addr);
        if ((ip >> 16) == 0xA9FE) continue;  // 169.254.x.x — chưa có DHCP
        int score = 0;
        if (!isVirtual) score += 100;
        if (ia.wireless || lower == "en0" || lower.rfind("wlan", 0) == 0 || lower.rfind("wlp", 0) == 0) score += 40;
        if ((ip >> 16) == 0xC0A8) score += 20;                                   // 192.168.x.x
        else if ((ip >> 24) == 10 || (ip >> 20) == 0xAC1) score += 10;           // 10.x, 172.16-31.x
        const std::string text = ipToString(ia.addr);
        if (text == routed) score += 5;
        if (score > bestScore) {
            bestScore = score;
            best = text;
        }
    }
    return best.empty() ? primaryLocalIp() : best;
}

Socket listenTcp(uint16_t port, std::string* error, int backlog) {
    initialize();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) {
        if (error) *error = "socket(TCP): " + lastErrorString();
        return {};
    }
    Socket sock(s);
    configureCommon(s);
#ifdef _WIN32
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    setBuffers(s);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "bind(TCP): " + lastErrorString();
        return {};
    }
    if (::listen(s, backlog) != 0) {
        if (error) *error = "listen: " + lastErrorString();
        return {};
    }
    return sock;
}

Socket acceptTcp(const Socket& listener, int timeoutMs, Endpoint* peer) {
    if (!listener.valid()) return {};
    std::vector<bool> ready;
    if (waitReadable({listener.fd()}, timeoutMs, &ready) <= 0 || !ready[0]) return {};
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    socket_t s = ::accept(listener.fd(), reinterpret_cast<sockaddr*>(&addr), &len);
    if (s == kInvalidSocket) return {};
    Socket sock(s);
    configureCommon(s);
    setNonBlocking(s, false);
    setBuffers(s);
    if (peer) {
        peer->ip = ipToString(addr.sin_addr);
        peer->port = ntohs(addr.sin_port);
    }
    return sock;
}

Socket connectTcp(const std::string& ip, uint16_t port, int timeoutMs, std::string* error) {
    initialize();
    sockaddr_in addr{};
    if (!toSockaddr(ip, port, &addr)) {
        if (error) *error = "IP không hợp lệ: " + ip;
        return {};
    }
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) {
        if (error) *error = "socket(TCP): " + lastErrorString();
        return {};
    }
    Socket sock(s);
    configureCommon(s);
    setBuffers(s);
    setNonBlocking(s, true);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (!wouldBlock(lastErrorCode())) {
            if (error) *error = "connect: " + lastErrorString();
            return {};
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(s, &writeSet);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int r = ::select(static_cast<int>(s) + 1, nullptr, &writeSet, nullptr, &tv);
        if (r <= 0) {
            if (error) *error = r == 0 ? "hết thời gian kết nối" : "select: " + lastErrorString();
            return {};
        }
        int soError = 0;
        socklen_t len = sizeof(soError);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
        if (soError != 0) {
            if (error) *error = "connect: lỗi " + std::to_string(soError);
            return {};
        }
    }
    setNonBlocking(s, false);
    return sock;
}

void setIoTimeout(const Socket& sock, int timeoutMs) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeoutMs);
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
#endif
    ::setsockopt(sock.fd(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(sock.fd(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
}

bool sendAll(const Socket& sock, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        int chunk = static_cast<int>(std::min<size_t>(len, 1 << 20));
        auto n = ::send(sock.fd(), p, chunk, kSendFlags);
        if (n <= 0) {
#ifndef _WIN32
            if (n < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(const Socket& sock, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        int chunk = static_cast<int>(std::min<size_t>(len, 1 << 20));
        auto n = ::recv(sock.fd(), p, chunk, 0);
        if (n <= 0) {
#ifndef _WIN32
            if (n < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

int recvSome(const Socket& sock, void* data, size_t len) {
    for (;;) {
        auto n = ::recv(sock.fd(), static_cast<char*>(data), static_cast<int>(std::min<size_t>(len, 1 << 20)), 0);
#ifndef _WIN32
        if (n < 0 && errno == EINTR) continue;
#endif
        return static_cast<int>(n);
    }
}

void shutdownBoth(socket_t fd) {
    if (fd == kInvalidSocket) return;
#ifdef _WIN32
    ::shutdown(fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

int waitReadable(const std::vector<socket_t>& fds, int timeoutMs, std::vector<bool>* ready) {
    if (ready) ready->assign(fds.size(), false);
    fd_set set;
    FD_ZERO(&set);
    socket_t maxFd = 0;
    bool any = false;
    for (socket_t fd : fds) {
        if (fd == kInvalidSocket) continue;
        FD_SET(fd, &set);
        maxFd = std::max(maxFd, fd);
        any = true;
    }
    if (!any) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return 0;
    }
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = ::select(static_cast<int>(maxFd) + 1, &set, nullptr, nullptr, &tv);
    if (r > 0 && ready) {
        for (size_t i = 0; i < fds.size(); ++i) {
            (*ready)[i] = fds[i] != kInvalidSocket && FD_ISSET(fds[i], &set);
        }
    }
    return r;
}

}  // namespace filedrop::net
