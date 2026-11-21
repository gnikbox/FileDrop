#include "filedrop/transfer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "filedrop/util.h"

#ifndef _WIN32
#include <errno.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace filedrop {

namespace {

using Clock = std::chrono::steady_clock;

constexpr char kMagic[4] = {'F', 'D', 'T', 'X'};
constexpr size_t kChunkSize = 256 * 1024;
constexpr int kIoTimeoutMs = 30000;
constexpr int kHandshakeTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr uint32_t kMaxFiles = 1000000;
constexpr uint32_t kMaxPathLength = 4096;
constexpr int kMaxPasswordAttempts = 5;

fs::path pathFromUtf8(const std::string& s) { return fs::u8path(s); }
std::string pathToUtf8(const fs::path& p) { return p.u8string(); }

void putU32(std::string& buf, uint32_t v) {
    for (int i = 3; i >= 0; --i) buf.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}
void putU64(std::string& buf, uint64_t v) {
    for (int i = 7; i >= 0; --i) buf.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}
bool recvU32(const net::Socket& s, uint32_t& v) {
    unsigned char b[4];
    if (!net::recvAll(s, b, sizeof(b))) return false;
    v = 0;
    for (unsigned char c : b) v = (v << 8) | c;
    return true;
}
bool recvU64(const net::Socket& s, uint64_t& v) {
    unsigned char b[8];
    if (!net::recvAll(s, b, sizeof(b))) return false;
    v = 0;
    for (unsigned char c : b) v = (v << 8) | c;
    return true;
}

// Gọi progress tối đa ~10 lần/giây, luôn gọi ở lần cuối.
class Throttle {
public:
    explicit Throttle(const ProgressFn& fn) : fn_(fn) {}
    void report(uint64_t done, uint64_t total, bool force = false) {
        if (!fn_) return;
        auto now = Clock::now();
        if (force || done >= total || now - last_ >= std::chrono::milliseconds(100)) {
            last_ = now;
            fn_(std::min(done, total), total);
        }
    }

private:
    const ProgressFn& fn_;
    Clock::time_point last_{};
};

TransferOutcome fail(std::string message) {
    TransferOutcome out;
    out.error = std::move(message);
    return out;
}

bool isReservedWindowsName(const std::string& name) {
    std::string upper;
    for (char c : name.substr(0, name.find('.'))) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                      "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                      "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* r : kReserved) {
        if (upper == r) return true;
    }
    return false;
}

// Tách đường dẫn tương đối nhận từ mạng thành các thành phần an toàn
// (loại bỏ "..", ".", ký tự không hợp lệ) để không thể ghi ra ngoài thư mục đích.
std::vector<std::string> sanitizeComponents(const std::string& rel) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '.')) cur.pop_back();
        if (!cur.empty() && cur != "." && cur != "..") {
            if (isReservedWindowsName(cur)) cur = "_" + cur;
            out.push_back(cur);
        }
        cur.clear();
    };
    for (char c : rel) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\') {
            flush();
        } else if (uc < 32 || std::strchr("<>:\"|?*", c) != nullptr) {
            cur.push_back('_');
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

fs::path uniquePath(const fs::path& dir, const std::string& name) {
    fs::path candidate = dir / pathFromUtf8(name);
    std::error_code ec;
    if (!fs::exists(candidate, ec)) return candidate;
    fs::path base = pathFromUtf8(name);
    std::string stem = pathToUtf8(base.stem());
    std::string ext = pathToUtf8(base.extension());
    for (int i = 1; i < 100000; ++i) {
        candidate = dir / pathFromUtf8(stem + " (" + std::to_string(i) + ")" + ext);
        if (!fs::exists(candidate, ec)) return candidate;
    }
    return dir / pathFromUtf8(stem + " (" + randomHex(4) + ")" + ext);
}

}  // namespace

#ifndef _WIN32
FileEntry fileEntryFromFd(int fd, const std::string& name, uint64_t size) {
    FileEntry e;
    e.absPath = name;
    auto parts = sanitizeComponents(name);
    e.relPath = parts.empty() ? std::string("file") : parts.back();
    e.size = size;
    e.fd = fd;
    e.fdOwner = std::shared_ptr<void>(nullptr, [fd](void*) { ::close(fd); });
    return e;
}
#endif

std::vector<FileEntry> collectFiles(const std::vector<std::string>& paths) {
    std::vector<FileEntry> files;
    for (const auto& raw : paths) {
        fs::path p = pathFromUtf8(raw);
        std::error_code ec;
        auto status = fs::status(p, ec);
        if (ec) continue;

        if (fs::is_regular_file(status)) {
            FileEntry e;
            e.absPath = pathToUtf8(fs::absolute(p, ec));
            e.relPath = pathToUtf8(p.filename());
            e.size = fs::file_size(p, ec);
            if (!ec) files.push_back(std::move(e));
        } else if (fs::is_directory(status)) {
            fs::path root = p;
            if (root.filename().empty()) root = root.parent_path();  // bỏ dấu '/' cuối
            fs::path base = root.filename();
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                const auto& entry = *it;
                std::error_code fec;
                if (!entry.is_regular_file(fec)) continue;
                std::string name = pathToUtf8(entry.path().filename());
                if (name == ".DS_Store" || name == "Thumbs.db" || name == "desktop.ini") continue;
                FileEntry e;
                e.absPath = pathToUtf8(entry.path());
                e.relPath = (base / entry.path().lexically_relative(root)).generic_u8string();
                e.size = entry.file_size(fec);
                if (!fec) files.push_back(std::move(e));
            }
        }
    }
    return files;
}

TransferOutcome runSender(const std::string& ip, uint16_t port, const std::string& password,
                          const std::vector<FileEntry>& files, const ProgressFn& progress,
                          const std::atomic<bool>& cancel) {
    if (password.size() != kPasswordLength) return fail("Mật khẩu phiên truyền không hợp lệ");

    std::string err;
    net::Socket sock = net::connectTcp(ip, port, kConnectTimeoutMs, &err);
    if (!sock.valid()) return fail("Không kết nối được tới máy nhận (" + err + ")");
    net::setIoTimeout(sock, kHandshakeTimeoutMs);

    std::string hello(kMagic, sizeof(kMagic));
    hello += password;
    if (!net::sendAll(sock, hello.data(), hello.size())) return fail("Mất kết nối khi gửi mật khẩu");

    char ack = 0;
    if (!net::recvAll(sock, &ack, 1)) return fail("Máy nhận không phản hồi");
    if (ack != 'A') return fail("Máy nhận từ chối mật khẩu");

    net::setIoTimeout(sock, kIoTimeoutMs);
    uint64_t total = 0;
    for (const auto& f : files) total += f.size;

    std::string header;
    putU32(header, static_cast<uint32_t>(files.size()));
    putU64(header, total);
    if (!net::sendAll(sock, header.data(), header.size())) return fail("Mất kết nối");

    Throttle throttle(progress);
    throttle.report(0, total, true);

    std::vector<char> buf(kChunkSize);
    uint64_t done = 0;
    for (const auto& f : files) {
        if (cancel) return fail("Đã hủy");
        std::ifstream in;
#ifndef _WIN32
        if (f.fd >= 0) {
            if (::lseek(f.fd, 0, SEEK_SET) < 0 && errno != ESPIPE) return fail("Không đọc được file: " + f.relPath);
        } else
#endif
        {
            in.open(pathFromUtf8(f.absPath), std::ios::binary);
            if (!in) return fail("Không đọc được file: " + f.relPath);
        }
        // Đọc đúng n byte từ fd hoặc ifstream.
        auto readExact = [&](char* dst, size_t n) -> bool {
#ifndef _WIN32
            if (f.fd >= 0) {
                while (n > 0) {
                    ssize_t r = ::read(f.fd, dst, n);
                    if (r < 0 && errno == EINTR) continue;
                    if (r <= 0) return false;
                    dst += r;
                    n -= static_cast<size_t>(r);
                }
                return true;
            }
#endif
            in.read(dst, static_cast<std::streamsize>(n));
            return static_cast<size_t>(in.gcount()) == n;
        };

        std::string fh;
        putU32(fh, static_cast<uint32_t>(f.relPath.size()));
        fh += f.relPath;
        putU64(fh, f.size);
        if (!net::sendAll(sock, fh.data(), fh.size())) return fail("Mất kết nối");

        uint64_t remaining = f.size;
        while (remaining > 0) {
            if (cancel) return fail("Đã hủy");
            size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkSize));
            if (!readExact(buf.data(), n)) return fail("File bị thay đổi trong lúc gửi: " + f.relPath);
            if (!net::sendAll(sock, buf.data(), n)) return fail("Mất kết nối trong lúc gửi");
            remaining -= n;
            done += n;
            throttle.report(done, total);
        }
    }

    char fin = 0;
    if (!net::recvAll(sock, &fin, 1) || fin != 'D') return fail("Máy nhận không xác nhận hoàn tất");
    throttle.report(total, total, true);

    TransferOutcome out;
    out.ok = true;
    out.bytes = total;
    return out;
}

TransferOutcome runReceiver(net::Socket& listener, const std::string& password, const std::string& destDir,
                            int acceptTimeoutMs, const std::function<void()>& onAuthenticated,
                            const ProgressFn& progress, const std::atomic<bool>& cancel) {
    // 1. Chờ kết nối + xác thực mật khẩu.
    const auto deadline = Clock::now() + std::chrono::milliseconds(acceptTimeoutMs);
    net::Socket conn;
    int badAttempts = 0;
    while (!conn.valid()) {
        if (cancel) return fail("Đã hủy");
        if (Clock::now() > deadline) return fail("Hết thời gian chờ máy gửi kết nối");
        net::Endpoint peer;
        net::Socket candidate = net::acceptTcp(listener, 250, &peer);
        if (!candidate.valid()) continue;

        net::setIoTimeout(candidate, kHandshakeTimeoutMs);
        char hello[sizeof(kMagic) + kPasswordLength];
        if (!net::recvAll(candidate, hello, sizeof(hello))) continue;
        bool ok = std::memcmp(hello, kMagic, sizeof(kMagic)) == 0 &&
                  constantTimeEquals(std::string(hello + sizeof(kMagic), kPasswordLength), password);
        if (!ok) {
            char reject = 'R';
            net::sendAll(candidate, &reject, 1);
            if (++badAttempts >= kMaxPasswordAttempts) return fail("Quá nhiều lần thử sai mật khẩu");
            continue;
        }
        char accept = 'A';
        if (!net::sendAll(candidate, &accept, 1)) continue;
        conn = std::move(candidate);
    }
    listener.close();  // Chỉ phục vụ đúng 1 máy gửi.
    if (onAuthenticated) onAuthenticated();

    // 2. Nhận dữ liệu.
    net::setIoTimeout(conn, kIoTimeoutMs);
    uint32_t count = 0;
    uint64_t total = 0;
    if (!recvU32(conn, count) || !recvU64(conn, total)) return fail("Mất kết nối khi nhận thông tin file");
    if (count > kMaxFiles) return fail("Số lượng file không hợp lệ");

    std::error_code ec;
    fs::path dest = pathFromUtf8(destDir);
    fs::create_directories(dest, ec);
    auto space = fs::space(dest, ec);
    if (!ec && space.available < total) return fail("Không đủ dung lượng trống để nhận file");

    Throttle throttle(progress);
    throttle.report(0, total, true);

    TransferOutcome out;
    std::map<std::string, fs::path> topLevelDirs;  // tên thư mục gốc gửi sang -> thư mục đã tạo
    std::vector<char> buf(kChunkSize);
    uint64_t done = 0;

    for (uint32_t i = 0; i < count; ++i) {
        if (cancel) return fail("Đã hủy");
        uint32_t pathLen = 0;
        if (!recvU32(conn, pathLen) || pathLen == 0 || pathLen > kMaxPathLength) return fail("Dữ liệu không hợp lệ");
        std::string rel(pathLen, '\0');
        uint64_t size = 0;
        if (!net::recvAll(conn, rel.data(), pathLen) || !recvU64(conn, size)) return fail("Mất kết nối");

        auto parts = sanitizeComponents(rel);
        if (parts.empty()) parts.push_back("file");

        fs::path target;
        if (parts.size() == 1) {
            target = uniquePath(dest, parts[0]);
            out.savedPaths.push_back(pathToUtf8(target));
        } else {
            auto it = topLevelDirs.find(parts[0]);
            if (it == topLevelDirs.end()) {
                fs::path dir = uniquePath(dest, parts[0]);
                it = topLevelDirs.emplace(parts[0], dir).first;
                out.savedPaths.push_back(pathToUtf8(dir));
            }
            target = it->second;
            for (size_t k = 1; k < parts.size(); ++k) target /= pathFromUtf8(parts[k]);
            fs::create_directories(target.parent_path(), ec);
            if (fs::exists(target, ec)) target = uniquePath(target.parent_path(), parts.back());
        }

        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file) return fail("Không ghi được file: " + pathToUtf8(target));

        uint64_t remaining = size;
        bool failed = false;
        std::string failure;
        while (remaining > 0) {
            if (cancel) { failed = true; failure = "Đã hủy"; break; }
            size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkSize));
            if (!net::recvAll(conn, buf.data(), n)) { failed = true; failure = "Mất kết nối trong lúc nhận"; break; }
            file.write(buf.data(), static_cast<std::streamsize>(n));
            if (!file) { failed = true; failure = "Lỗi ghi đĩa"; break; }
            remaining -= n;
            done += n;
            throttle.report(done, total);
        }
        file.close();
        if (failed) {
            fs::remove(target, ec);
            return fail(failure);
        }
    }

    char fin = 'D';
    net::sendAll(conn, &fin, 1);
    throttle.report(total, total, true);
    out.ok = true;
    out.bytes = done;
    return out;
}

}  // namespace filedrop
