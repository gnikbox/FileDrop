#include "filedrop/webserver.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "filedrop/net.h"
#include "filedrop/transfer.h"
#include "filedrop/util.h"
#include "web_assets.h"

namespace fs = std::filesystem;

namespace filedrop {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxSmallBody = 4 * 1024;
constexpr int kHeaderTimeoutMs = 15000;
constexpr int kBodyTimeoutMs = 30000;
constexpr auto kLongPoll = std::chrono::seconds(20);
constexpr size_t kMaxConnections = 64;
constexpr size_t kMaxClients = 64;
constexpr int kMaxPendingPerIp = 3;
constexpr auto kPendingTimeout = std::chrono::seconds(60);   // trang chờ duyệt đã đóng
constexpr auto kForgetClosedAfter = std::chrono::minutes(10);
constexpr auto kBatchIdleTimeout = std::chrono::seconds(90);
constexpr size_t kChunkSize = 256 * 1024;
constexpr size_t kMaxNameLength = 64;

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2])));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Tên máy khách tự nhập: bỏ ký tự điều khiển, cắt ngắn (không cắt giữa ký tự UTF-8).
std::string cleanName(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) s.push_back(c);
    }
    s = trim(s);
    if (s.size() > kMaxNameLength) {
        size_t cut = kMaxNameLength;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
    }
    return s;
}

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        case 507: return "Insufficient Storage";
        default: return "Error";
    }
}

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;  // tên header viết thường
    std::string leftover;                        // phần body đã đọc lẫn cùng header
    bool hasLength = false;
    uint64_t contentLength = 0;

    std::string q(const std::string& key) const {
        auto it = query.find(key);
        return it == query.end() ? std::string() : it->second;
    }
};

bool readRequest(const net::Socket& sock, Request& req) {
    std::string buf;
    char tmp[4096];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > kMaxHeaderBytes) return false;
        int n = net::recvSome(sock, tmp, sizeof(tmp));
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
    req.leftover = buf.substr(headerEnd + 4);
    std::string head = buf.substr(0, headerEnd);

    size_t lineEnd = head.find("\r\n");
    std::string first = head.substr(0, lineEnd);
    size_t sp1 = first.find(' ');
    size_t sp2 = first.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = first.substr(0, sp1);
    std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qpos = target.find('?');
    req.path = urlDecode(target.substr(0, qpos));
    if (qpos != std::string::npos) {
        std::string qs = target.substr(qpos + 1);
        size_t start = 0;
        while (start <= qs.size()) {
            size_t amp = qs.find('&', start);
            std::string part = qs.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            if (!part.empty()) {
                size_t eq = part.find('=');
                std::string k = urlDecode(part.substr(0, eq));
                std::string v = eq == std::string::npos ? std::string() : urlDecode(part.substr(eq + 1));
                req.query[k] = v;
            }
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
    }

    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) end = head.size();
        std::string line = head.substr(pos, end - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) req.headers[toLower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        pos = end + 2;
    }
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
        const std::string& v = it->second;
        if (v.empty() || v.size() > 19 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        req.hasLength = true;
        req.contentLength = std::stoull(v);
    }
    // Không hỗ trợ chunked: trình duyệt luôn gửi Content-Length với fetch/XHR.
    if (auto it = req.headers.find("transfer-encoding"); it != req.headers.end() && toLower(it->second) != "identity")
        return false;
    return true;
}

// Đọc body nhỏ (tên máy...).
bool readSmallBody(const net::Socket& sock, Request& req, std::string& body) {
    if (!req.hasLength) {
        body.clear();
        return true;
    }
    if (req.contentLength > kMaxSmallBody) return false;
    body = req.leftover.substr(0, static_cast<size_t>(req.contentLength));
    size_t need = static_cast<size_t>(req.contentLength) - body.size();
    if (need > 0) {
        std::string rest(need, '\0');
        if (!net::recvAll(sock, rest.data(), need)) return false;
        body += rest;
    }
    return true;
}

void sendResponse(const net::Socket& sock, int code, const std::string& contentType, const std::string& body,
                  const std::string& extraHeaders = {}) {
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + statusText(code) + "\r\n";
    if (!contentType.empty()) head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n";
    head += "Referrer-Policy: no-referrer\r\n";
    head += extraHeaders;
    head += "\r\n";
    net::sendAll(sock, head.data(), head.size());
    if (!body.empty()) net::sendAll(sock, body.data(), body.size());
}

void sendJson(const net::Socket& sock, int code, const std::string& json) {
    sendResponse(sock, code, "application/json; charset=utf-8", json);
}

void sendError(const net::Socket& sock, int code, const std::string& error) {
    sendJson(sock, code, "{\"error\":\"" + jsonEscape(error) + "\"}");
}

std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

const char* downloadContentType(const std::string& fileName) {
    std::string ext = toLower(fs::u8path(fileName).extension().u8string());
    if (ext == ".apk") return "application/vnd.android.package-archive";
    if (ext == ".zip") return "application/zip";
    if (ext == ".dmg") return "application/x-apple-diskimage";
    if (ext == ".msi") return "application/x-msi";
    return "application/octet-stream";
}

// Kích thước file bản cài; -1 nếu không có.
int64_t downloadSize(const WebDownload& d) {
    std::error_code ec;
    fs::path p = fs::u8path(d.path);
    if (!fs::is_regular_file(p, ec)) return -1;
    auto n = fs::file_size(p, ec);
    return ec ? -1 : static_cast<int64_t>(n);
}

std::string platformForFile(const std::string& fileName) {
    std::string name = toLower(fileName);
    std::string ext = toLower(fs::u8path(fileName).extension().u8string());
    if (ext == ".apk") return "android";
    if (ext == ".exe" || ext == ".msi") return "windows";
    if (ext == ".dmg") return "macos";
    if (ext == ".appimage" || ext == ".deb") return "linux";
    if (ext == ".zip") {
        if (name.find("mac") != std::string::npos || name.find("osx") != std::string::npos) return "macos";
        if (name.find("win") != std::string::npos) return "windows";
        if (name.find("linux") != std::string::npos) return "linux";
        if (name.find("android") != std::string::npos) return "android";
    }
    return {};
}

const char* stateName(WebClientState s) {
    switch (s) {
        case WebClientState::Pending: return "pending";
        case WebClientState::Connected: return "accepted";
        case WebClientState::Declined: return "declined";
        case WebClientState::Disconnected: return "closed";
    }
    return "closed";
}

}  // namespace

struct WebServer::Impl {
    using Event = std::function<void()>;

    struct Client {
        WebClient info;
        std::string token;
        Clock::time_point lastSeen;
        Clock::time_point closedAt;
    };
    struct Batch {
        TransferInfo info;
        std::string clientId;
        int expected = 0;
        int received = 0;
        uint64_t done = 0;
        std::unique_ptr<SaveLocator> locator;
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        Clock::time_point lastActivity;
        Clock::time_point lastReport{};
        int uploading = 0;
        std::string saveDir;
    };

    WebServerCallbacks cb;

    mutable std::mutex mu;
    std::condition_variable stateCv;  // đánh thức long-poll khi trạng thái máy khách đổi
    std::map<std::string, Client> clients;
    std::map<std::string, std::string> tokens;  // token -> clientId
    std::map<std::string, Batch> batches;
    std::string hostName;
    std::string downloadDir;
    std::string url;
    std::vector<WebDownload> downloads;
    std::vector<std::string> packageDirs;

    net::Socket listener;
    uint16_t port = 0;
    std::atomic<bool> running{false};
    std::thread acceptThread;

    std::mutex connMu;
    std::condition_variable connCv;
    std::set<net::socket_t> conns;

    explicit Impl(WebServerCallbacks c) : cb(std::move(c)) {}

    static void fire(std::vector<Event>& events) {
        for (auto& e : events) {
            if (e) e();
        }
        events.clear();
    }

    Event clientEvent(const WebClient& c) {
        return [this, c] {
            if (cb.onClientChanged) cb.onClientChanged(c);
        };
    }

    // Gọi khi đang giữ mu. Xoá batch và trả về sự kiện kết thúc.
    Event finishBatchLocked(const std::string& batchId, TransferStatus status, std::string message) {
        auto it = batches.find(batchId);
        if (it == batches.end()) return {};
        it->second.cancel->store(true);
        TransferInfo info = it->second.info;
        TransferResult r;
        r.status = status;
        r.message = std::move(message);
        r.saveDir = it->second.saveDir;
        if (status == TransferStatus::Completed && it->second.locator) r.savedPaths = it->second.locator->topLevelPaths();
        batches.erase(it);
        return [this, info, r] {
            if (cb.onTransferFinished) cb.onTransferFinished(info, r);
        };
    }

    // Gọi khi đang giữ mu.
    void closeClientLocked(const std::string& clientId, WebClientState state, std::vector<Event>& events,
                           const std::string& reason) {
        auto it = clients.find(clientId);
        if (it == clients.end()) return;
        Client& c = it->second;
        if (c.info.state == WebClientState::Declined || c.info.state == WebClientState::Disconnected) return;
        c.info.state = state;
        c.closedAt = Clock::now();
        tokens.erase(c.token);
        std::vector<std::string> ids;
        for (auto& [id, b] : batches) {
            if (b.clientId == clientId) ids.push_back(id);
        }
        for (auto& id : ids) events.push_back(finishBatchLocked(id, TransferStatus::Cancelled, reason));
        events.push_back(clientEvent(c.info));
        stateCv.notify_all();
    }

    // Tìm máy khách theo token; IP phải khớp với IP đã chào.
    Client* clientFor(const Request& req, const std::string& ip) {
        auto t = tokens.find(req.q("s"));
        if (t == tokens.end()) return nullptr;
        auto it = clients.find(t->second);
        if (it == clients.end() || it->second.info.ip != ip) return nullptr;
        it->second.lastSeen = Clock::now();
        return &it->second;
    }

    // ---- Vòng nhận kết nối ----

    void acceptLoop() {
        while (running) {
            net::Endpoint peer;
            net::Socket sock = net::acceptTcp(listener, 250, &peer);
            housekeeping();
            if (!sock.valid()) continue;
            {
                std::lock_guard<std::mutex> l(connMu);
                if (conns.size() >= kMaxConnections) {
                    sendError(sock, 503, "busy");
                    continue;
                }
                conns.insert(sock.fd());
            }
            std::thread([this, s = std::move(sock), ip = peer.ip]() mutable {
                handleConnection(s, ip);
                std::lock_guard<std::mutex> l(connMu);
                conns.erase(s.fd());
                s.close();
                connCv.notify_all();
            }).detach();
        }
    }

    void housekeeping() {
        std::vector<Event> events;
        {
            std::lock_guard<std::mutex> l(mu);
            auto now = Clock::now();
            std::vector<std::string> stale;
            for (auto& [id, b] : batches) {
                if (b.uploading == 0 && now - b.lastActivity > kBatchIdleTimeout) stale.push_back(id);
            }
            for (auto& id : stale)
                events.push_back(finishBatchLocked(id, TransferStatus::Failed, "Máy gửi không phản hồi"));

            std::vector<std::string> gone;
            for (auto& [id, c] : clients) {
                if (c.info.state == WebClientState::Pending && now - c.lastSeen > kPendingTimeout) gone.push_back(id);
            }
            for (auto& id : gone) closeClientLocked(id, WebClientState::Disconnected, events, "Máy gửi đã rời đi");

            for (auto it = clients.begin(); it != clients.end();) {
                bool closed = it->second.info.state == WebClientState::Declined ||
                              it->second.info.state == WebClientState::Disconnected;
                if (closed && now - it->second.closedAt > kForgetClosedAfter) {
                    it = clients.erase(it);
                } else {
                    ++it;
                }
            }
        }
        fire(events);
    }

    // ---- Xử lý request ----

    void handleConnection(net::Socket& sock, const std::string& ip) {
        net::setIoTimeout(sock, kHeaderTimeoutMs);
        Request req;
        if (!readRequest(sock, req)) {
            sendError(sock, 400, "bad request");
            return;
        }
        const std::string& p = req.path;
        const std::string& m = req.method;

        if (p == "/" || p == "/index.html") {
            if (m != "GET" && m != "HEAD") return sendError(sock, 405, "method");
            std::string page(reinterpret_cast<const char*>(kWebIndexHtml), kWebIndexHtmlSize);
            sendResponse(sock, 200, "text/html; charset=utf-8", m == "HEAD" ? std::string() : page,
                         "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; "
                         "style-src 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; "
                         "frame-ancestors 'none'\r\n");
            return;
        }
        if (p == "/favicon.ico") return sendResponse(sock, 204, "", "");
        if (p == "/api/info" && m == "GET") return handleInfo(sock);
        if (p == "/api/hello" && m == "POST") return handleHello(sock, req, ip);
        if (p == "/api/status" && m == "GET") return handleStatus(sock, req, ip);
        if (p == "/api/bye" && m == "POST") return handleBye(sock, req, ip);
        if (p == "/api/batch" && m == "POST") return handleBatch(sock, req, ip);
        if (p == "/api/batch/end" && m == "POST") return handleBatchEnd(sock, req, ip);
        if (p == "/api/upload" && m == "PUT") return handleUpload(sock, req, ip);
        if (p.rfind("/download/", 0) == 0 && (m == "GET" || m == "HEAD"))
            return handleDownload(sock, p.substr(10), m == "HEAD");
        sendError(sock, 404, "not found");
    }

    // Danh sách bản cài thực có: mỗi nền tảng một bản, file thiếu bị bỏ qua.
    std::vector<WebDownload> availableDownloads() {
        std::vector<WebDownload> list;
        std::vector<std::string> dirs;
        {
            std::lock_guard<std::mutex> l(mu);
            list = downloads;
            dirs = packageDirs;
        }
        for (auto& d : dirs) {
            auto found = scanWebDownloads(d);
            list.insert(list.end(), found.begin(), found.end());
        }
        std::vector<WebDownload> out;
        std::set<std::string> seen;
        for (auto& d : list) {
            if (d.platform.empty() || seen.count(d.platform) || downloadSize(d) < 0) continue;
            seen.insert(d.platform);
            out.push_back(d);
        }
        return out;
    }

    void handleInfo(net::Socket& sock) {
        std::string name;
        {
            std::lock_guard<std::mutex> l(mu);
            name = hostName;
        }
        std::vector<WebDownload> list = availableDownloads();
        std::string apps;
        for (auto& d : list) {
            int64_t size = downloadSize(d);
            if (size < 0) continue;
            if (!apps.empty()) apps += ",";
            apps += "{\"platform\":\"" + jsonEscape(d.platform) + "\",\"name\":\"" + jsonEscape(d.fileName) +
                    "\",\"size\":" + std::to_string(size) + "}";
        }
        sendJson(sock, 200, "{\"host\":\"" + jsonEscape(name) + "\",\"os\":\"" + jsonEscape(currentOs()) +
                                "\",\"apps\":[" + apps + "]}");
    }

    // GET /download/<platform>: tải bản cài FileDrop (không cần được duyệt — chỉ là file cài của chính app).
    void handleDownload(net::Socket& sock, const std::string& platform, bool headOnly) {
        WebDownload d;
        bool found = false;
        for (auto& x : availableDownloads()) {
            if (x.platform == platform) {
                d = x;
                found = true;
                break;
            }
        }
        if (!found) return sendError(sock, 404, "not found");
        std::ifstream in(fs::u8path(d.path), std::ios::binary);
        int64_t size = downloadSize(d);
        if (!in || size < 0) return sendError(sock, 404, "not found");

        std::string fileName = d.fileName.empty() ? fs::u8path(d.path).filename().u8string() : d.fileName;
        std::string ascii;
        for (unsigned char c : fileName) ascii.push_back(c >= 0x20 && c < 0x7f && c != '"' && c != '\\' ? c : '_');
        std::string head = "HTTP/1.1 200 OK\r\n";
        head += std::string("Content-Type: ") + downloadContentType(fileName) + "\r\n";
        head += "Content-Length: " + std::to_string(size) + "\r\n";
        head += "Content-Disposition: attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" +
                percentEncode(fileName) + "\r\n";
        head += "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
        if (!net::sendAll(sock, head.data(), head.size()) || headOnly) return;

        net::setIoTimeout(sock, kBodyTimeoutMs);
        std::vector<char> buf(kChunkSize);
        uint64_t left = static_cast<uint64_t>(size);
        while (left > 0 && running) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
            in.read(buf.data(), static_cast<std::streamsize>(want));
            size_t got = static_cast<size_t>(in.gcount());
            if (got == 0) return;  // file bị đổi giữa chừng: đóng kết nối, trình duyệt báo lỗi
            if (!net::sendAll(sock, buf.data(), got)) return;
            left -= got;
        }
    }

    void handleHello(net::Socket& sock, Request& req, const std::string& ip) {
        std::string body;
        if (!readSmallBody(sock, req, body)) return sendError(sock, 400, "bad body");
        std::string name = cleanName(body);
        if (name.empty()) name = ip;

        std::vector<Event> events;
        std::string token, id;
        {
            std::unique_lock<std::mutex> l(mu);
            int pendingFromIp = 0;
            size_t open = 0;
            for (auto& [cid, c] : clients) {
                if (c.info.state == WebClientState::Pending && c.info.ip == ip) ++pendingFromIp;
                if (c.info.state == WebClientState::Pending || c.info.state == WebClientState::Connected) ++open;
            }
            if (pendingFromIp >= kMaxPendingPerIp || open >= kMaxClients) {
                l.unlock();
                return sendError(sock, 429, "too many requests");
            }
            Client c;
            c.info.id = randomHex(8);
            c.info.name = name;
            c.info.ip = ip;
            c.info.state = WebClientState::Pending;
            c.token = randomHex(16);
            c.lastSeen = Clock::now();
            id = c.info.id;
            token = c.token;
            tokens[token] = id;
            events.push_back(clientEvent(c.info));
            clients.emplace(id, std::move(c));
        }
        sendJson(sock, 200, "{\"session\":\"" + token + "\",\"client\":\"" + id + "\"}");
        fire(events);
    }

    void handleStatus(net::Socket& sock, Request& req, const std::string& ip) {
        std::string known = req.q("known");
        std::string state;
        {
            std::unique_lock<std::mutex> l(mu);
            auto deadline = Clock::now() + kLongPoll;
            for (;;) {
                // Token đã bị xoá (từ chối/ngắt) vẫn trả về trạng thái cuối qua clientId nếu còn nhớ.
                Client* c = clientFor(req, ip);
                if (!c) {
                    auto it = clients.find(req.q("c"));
                    bool match = it != clients.end() && it->second.info.ip == ip &&
                                 constantTimeEquals(it->second.token, req.q("s"));
                    state = match ? stateName(it->second.info.state) : "closed";
                } else {
                    state = stateName(c->info.state);
                }
                if (!running) state = "closed";
                if (known.empty() || state != known || !running) break;
                if (stateCv.wait_until(l, deadline) == std::cv_status::timeout) break;
            }
        }
        sendJson(sock, 200, "{\"state\":\"" + state + "\"}");
    }

    void handleBye(net::Socket& sock, Request& req, const std::string& ip) {
        std::string body;
        readSmallBody(sock, req, body);
        std::vector<Event> events;
        {
            std::lock_guard<std::mutex> l(mu);
            if (Client* c = clientFor(req, ip))
                closeClientLocked(c->info.id, WebClientState::Disconnected, events, "Máy gửi đã ngắt kết nối");
        }
        sendJson(sock, 200, "{\"ok\":true}");
        fire(events);
    }

    void handleBatch(net::Socket& sock, Request& req, const std::string& ip) {
        std::string body;
        readSmallBody(sock, req, body);
        int count = 0;
        uint64_t total = 0;
        try {
            count = std::stoi(req.q("count"));
            total = std::stoull(req.q("total"));
        } catch (...) {
            return sendError(sock, 400, "bad batch");
        }
        if (count <= 0 || count > 100000) return sendError(sock, 400, "bad batch");

        std::vector<Event> events;
        std::string batchId;
        {
            std::unique_lock<std::mutex> l(mu);
            Client* c = clientFor(req, ip);
            if (!c || c->info.state != WebClientState::Connected) {
                l.unlock();
                return sendError(sock, 403, "not accepted");
            }
            std::error_code ec;
            fs::path dest = fs::u8path(downloadDir);
            fs::create_directories(dest, ec);
            auto space = fs::space(dest, ec);
            if (!ec && space.available < total) {
                l.unlock();
                return sendError(sock, 507, "no space");
            }
            // Trang tải lại giữa chừng: batch cũ của máy này coi như bị huỷ.
            std::vector<std::string> old;
            for (auto& [id, b] : batches) {
                if (b.clientId == c->info.id) old.push_back(id);
            }
            for (auto& id : old) events.push_back(finishBatchLocked(id, TransferStatus::Cancelled, "Máy gửi đã hủy"));

            Batch b;
            b.info.id = randomHex(8);
            b.info.direction = Direction::Receive;
            b.info.peer.id = c->info.id;
            b.info.peer.name = c->info.name;
            b.info.peer.os = "web";
            b.info.peer.ip = c->info.ip;
            b.info.fileCount = count;
            b.info.fileName = count == 1 ? req.q("name") : std::string();
            b.info.totalBytes = total;
            b.clientId = c->info.id;
            b.expected = count;
            b.saveDir = downloadDir;
            b.locator = std::make_unique<SaveLocator>(downloadDir);
            b.lastActivity = Clock::now();
            batchId = b.info.id;
            TransferInfo info = b.info;
            batches.emplace(batchId, std::move(b));
            events.push_back([this, info] {
                if (cb.onTransferPhase) cb.onTransferPhase(info, TransferPhase::Transferring);
                if (cb.onProgress) cb.onProgress(info, 0, info.totalBytes);
            });
        }
        sendJson(sock, 200, "{\"batch\":\"" + batchId + "\"}");
        fire(events);
    }

    void handleBatchEnd(net::Socket& sock, Request& req, const std::string& ip) {
        std::string body;
        readSmallBody(sock, req, body);
        std::vector<Event> events;
        {
            std::lock_guard<std::mutex> l(mu);
            Client* c = clientFor(req, ip);
            auto it = batches.find(req.q("b"));
            if (c && it != batches.end() && it->second.clientId == c->info.id)
                events.push_back(finishBatchLocked(it->first, TransferStatus::Cancelled, "Máy gửi đã hủy"));
        }
        sendJson(sock, 200, "{\"ok\":true}");
        fire(events);
    }

    void handleUpload(net::Socket& sock, Request& req, const std::string& ip) {
        if (!req.hasLength) return sendError(sock, 411, "length required");
        const uint64_t size = req.contentLength;
        const std::string batchId = req.q("b");

        std::string target;
        std::shared_ptr<std::atomic<bool>> cancel;
        TransferInfo info;
        uint64_t base = 0;
        {
            std::unique_lock<std::mutex> l(mu);
            Client* c = clientFor(req, ip);
            auto it = batches.find(batchId);
            if (!c || c->info.state != WebClientState::Connected || it == batches.end() ||
                it->second.clientId != c->info.id) {
                l.unlock();
                return sendError(sock, 403, "cancelled");
            }
            Batch& b = it->second;
            if (b.received >= b.expected || b.done + size > b.info.totalBytes) {
                l.unlock();
                return sendError(sock, 400, "size mismatch");
            }
            target = b.locator->resolve(req.q("path"));
            cancel = b.cancel;
            info = b.info;
            base = b.done;
            b.uploading++;
            b.lastActivity = Clock::now();
        }

        auto failUpload = [&](int code, const std::string& err, TransferStatus status, const std::string& msg) {
            std::error_code ec;
            fs::remove(fs::u8path(target), ec);
            std::vector<Event> events;
            {
                std::lock_guard<std::mutex> l(mu);
                auto it = batches.find(batchId);
                if (it != batches.end()) {
                    it->second.uploading--;
                    events.push_back(finishBatchLocked(batchId, status, msg));
                }
            }
            sendError(sock, code, err);
            fire(events);
        };

        std::ofstream file(fs::u8path(target), std::ios::binary | std::ios::trunc);
        if (!file) return failUpload(500, "write failed", TransferStatus::Failed, "Không ghi được file: " + target);

        net::setIoTimeout(sock, kBodyTimeoutMs);
        uint64_t written = 0;
        auto writeChunk = [&](const char* data, size_t n) -> bool {
            file.write(data, static_cast<std::streamsize>(n));
            if (!file) return false;
            written += n;
            reportProgress(batchId, info, base + written);
            return true;
        };

        if (!req.leftover.empty()) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(req.leftover.size(), size));
            if (!writeChunk(req.leftover.data(), n))
                return failUpload(500, "write failed", TransferStatus::Failed, "Lỗi ghi đĩa");
        }
        std::vector<char> buf(kChunkSize);
        while (written < size) {
            if (*cancel || !running) {
                return failUpload(409, "cancelled", TransferStatus::Cancelled,
                                  running ? "Đã hủy" : "Đã tắt máy chủ nhận file");
            }
            size_t want = static_cast<size_t>(std::min<uint64_t>(size - written, buf.size()));
            int n = net::recvSome(sock, buf.data(), want);
            if (n <= 0) {
                bool wasCancelled = *cancel || !running;
                return failUpload(400, "connection lost", wasCancelled ? TransferStatus::Cancelled : TransferStatus::Failed,
                                  wasCancelled ? "Đã hủy" : "Mất kết nối với máy gửi");
            }
            if (!writeChunk(buf.data(), static_cast<size_t>(n)))
                return failUpload(500, "write failed", TransferStatus::Failed, "Lỗi ghi đĩa");
        }
        file.close();
        if (!file) return failUpload(500, "write failed", TransferStatus::Failed, "Lỗi ghi đĩa");

        std::vector<Event> events;
        bool batchDone = false;
        {
            std::lock_guard<std::mutex> l(mu);
            auto it = batches.find(batchId);
            if (it == batches.end()) {
                // Bị huỷ đúng lúc vừa nhận xong: bỏ file.
                std::error_code ec;
                fs::remove(fs::u8path(target), ec);
            } else {
                Batch& b = it->second;
                b.uploading--;
                b.received++;
                b.done += written;
                b.lastActivity = Clock::now();
                if (b.received >= b.expected) {
                    batchDone = true;
                    TransferInfo bi = b.info;
                    uint64_t total = b.info.totalBytes;
                    events.push_back([this, bi, total] {
                        if (cb.onProgress) cb.onProgress(bi, total, total);
                    });
                    events.push_back(finishBatchLocked(batchId, TransferStatus::Completed, ""));
                }
            }
        }
        sendJson(sock, 200, std::string("{\"ok\":true,\"done\":") + (batchDone ? "true" : "false") + "}");
        fire(events);
    }

    // Báo tiến trình tối đa ~10 lần/giây cho mỗi batch.
    void reportProgress(const std::string& batchId, const TransferInfo& info, uint64_t done) {
        uint64_t total = 0;
        {
            std::lock_guard<std::mutex> l(mu);
            auto it = batches.find(batchId);
            if (it == batches.end()) return;
            auto now = Clock::now();
            it->second.lastActivity = now;
            if (now - it->second.lastReport < std::chrono::milliseconds(100)) return;
            it->second.lastReport = now;
            total = it->second.info.totalBytes;
        }
        if (cb.onProgress) cb.onProgress(info, std::min(done, total), total);
    }
};

WebServer::WebServer(WebServerCallbacks callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start(const std::string& hostName, const std::string& downloadDir, std::string* error) {
    if (impl_->running) return true;
    net::initialize();
    impl_->listener = net::listenTcp(0, error, 32);
    if (!impl_->listener.valid()) return false;
    impl_->port = net::localPort(impl_->listener);
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        impl_->hostName = hostName.empty() ? defaultDeviceName() : hostName;
        impl_->downloadDir = downloadDir.empty() ? defaultDownloadDir() : downloadDir;
        impl_->url = "http://" + net::lanIp() + ":" + std::to_string(impl_->port) + "/";
    }
    impl_->running = true;
    impl_->acceptThread = std::thread([this] { impl_->acceptLoop(); });
    return true;
}

void WebServer::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->acceptThread.joinable()) impl_->acceptThread.join();
    impl_->listener.close();

    std::vector<Impl::Event> events;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        std::vector<std::string> ids;
        for (auto& [id, c] : impl_->clients) ids.push_back(id);
        for (auto& id : ids)
            impl_->closeClientLocked(id, WebClientState::Disconnected, events, "Đã tắt máy chủ nhận file");
        impl_->stateCv.notify_all();
    }
    {
        // Đánh thức các kết nối đang chờ đọc/ghi rồi đợi chúng kết thúc.
        std::unique_lock<std::mutex> l(impl_->connMu);
        for (auto fd : impl_->conns) net::shutdownBoth(fd);
        impl_->connCv.wait(l, [this] { return impl_->conns.empty(); });
    }
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        std::vector<std::string> ids;
        for (auto& [id, b] : impl_->batches) ids.push_back(id);
        for (auto& id : ids)
            events.push_back(impl_->finishBatchLocked(id, TransferStatus::Cancelled, "Đã tắt máy chủ nhận file"));
        impl_->clients.clear();
        impl_->tokens.clear();
        impl_->url.clear();
    }
    impl_->port = 0;
    Impl::fire(events);
}

bool WebServer::isRunning() const { return impl_->running; }

uint16_t WebServer::port() const { return impl_->port; }

std::string WebServer::url() const {
    std::lock_guard<std::mutex> l(impl_->mu);
    return impl_->url;
}

bool WebServer::respond(const std::string& clientId, bool accept) {
    std::vector<Impl::Event> events;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        auto it = impl_->clients.find(clientId);
        if (it == impl_->clients.end() || it->second.info.state != WebClientState::Pending) return false;
        if (accept) {
            it->second.info.state = WebClientState::Connected;
            events.push_back(impl_->clientEvent(it->second.info));
            impl_->stateCv.notify_all();
        } else {
            impl_->closeClientLocked(clientId, WebClientState::Declined, events, "Đã từ chối");
        }
    }
    Impl::fire(events);
    return true;
}

void WebServer::disconnect(const std::string& clientId) {
    std::vector<Impl::Event> events;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        impl_->closeClientLocked(clientId, WebClientState::Disconnected, events, "Đã ngắt kết nối");
    }
    Impl::fire(events);
}

bool WebServer::cancel(const std::string& transferId) {
    std::vector<Impl::Event> events;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        auto it = impl_->batches.find(transferId);
        if (it == impl_->batches.end()) return false;
        events.push_back(impl_->finishBatchLocked(transferId, TransferStatus::Cancelled, "Đã hủy"));
    }
    Impl::fire(events);
    return true;
}

std::vector<WebClient> WebServer::clients() const {
    std::lock_guard<std::mutex> l(impl_->mu);
    std::vector<WebClient> out;
    for (auto& [id, c] : impl_->clients) {
        if (c.info.state == WebClientState::Pending || c.info.state == WebClientState::Connected) out.push_back(c.info);
    }
    return out;
}

void WebServer::setHostName(const std::string& name) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->hostName = name.empty() ? defaultDeviceName() : name;
}

std::vector<WebDownload> scanWebDownloads(const std::string& dir) {
    std::vector<WebDownload> out;
    if (dir.empty()) return out;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        std::string name = f.filename().u8string();
        if (name.empty() || name[0] == '.') continue;
        std::string platform = platformForFile(name);
        if (!platform.empty()) out.push_back({platform, f.u8string(), name});
    }
    return out;
}

void WebServer::setDownloads(std::vector<WebDownload> downloads, std::vector<std::string> packageDirs) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->downloads = std::move(downloads);
    impl_->packageDirs = std::move(packageDirs);
}

void WebServer::setDownloadDir(const std::string& dir) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->downloadDir = dir.empty() ? defaultDownloadDir() : dir;
}

}  // namespace filedrop
