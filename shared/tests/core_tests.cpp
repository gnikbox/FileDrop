// Kiểm thử lõi: mã hoá giao thức + truyền file end-to-end giữa 2 Engine trên cùng máy.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#endif

#include "filedrop/engine.h"
#include "filedrop/net.h"
#include "filedrop/protocol.h"
#include "filedrop/util.h"
#include "filedrop/webserver.h"

namespace fs = std::filesystem;
using namespace filedrop;

static int gFailures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++gFailures;                                                     \
        }                                                                    \
    } while (0)

static void testProtocol() {
    Message m(MessageType::SendRequest);
    m.set(field::kName, "Máy của Tuấn\nline2=x\\y");
    m.setUInt(field::kSize, 1234567890123ULL);
    m.setInt(field::kCount, 3);
    auto d = Message::decode(m.encode());
    CHECK(d.has_value());
    CHECK(d->type() == MessageType::SendRequest);
    CHECK(d->get(field::kName) == "Máy của Tuấn\nline2=x\\y");
    CHECK(d->getUInt(field::kSize) == 1234567890123ULL);
    CHECK(d->getInt(field::kCount) == 3);
    CHECK(!Message::decode("HELLO WORLD\n").has_value());
    CHECK(!Message::decode("FILEDROP/1 NOPE\n").has_value());
    CHECK(randomToken(16).size() == 16);
    CHECK(constantTimeEquals("abc", "abc") && !constantTimeEquals("abc", "abd"));
}

static std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void testEndToEnd() {
    const fs::path root = fs::temp_directory_path() / ("filedrop_test_" + randomHex(4));
    const fs::path src = root / "src";
    const fs::path dst = root / "dst";
    fs::create_directories(src / "Album" / "sub");
    fs::create_directories(dst);

    std::string big(3 * 1024 * 1024 + 17, '\0');
    std::mt19937 gen(42);
    for (auto& c : big) c = static_cast<char>(gen() & 0xFF);
    std::ofstream(src / "big.bin", std::ios::binary) << big;
    std::ofstream(src / "Album" / "a.txt") << "hello";
    std::ofstream(src / "Album" / "sub" / "b.txt") << "world";
    std::ofstream(dst / "big.bin") << "existing";  // kiểm tra đổi tên khi trùng

    const uint16_t port = 45999;
    std::mutex mu;
    std::condition_variable cv;
    bool senderDone = false, receiverDone = false, sawPeer = false;
    TransferStatus senderStatus = TransferStatus::Failed, receiverStatus = TransferStatus::Failed;
    std::string senderMsg, receiverMsg;
    Engine* receiverPtr = nullptr;

    EngineCallbacks rcb;
    rcb.onIncomingRequest = [&](const TransferInfo& info) {
        std::thread([&, id = info.id] { receiverPtr->respond(id, true); }).detach();
    };
    rcb.onTransferFinished = [&](const TransferInfo&, const TransferResult& r) {
        std::lock_guard<std::mutex> l(mu);
        receiverDone = true;
        receiverStatus = r.status;
        receiverMsg = r.message;
        cv.notify_all();
    };
    EngineCallbacks scb;
    scb.onPeerUpdated = [&](const Peer& p) {
        std::lock_guard<std::mutex> l(mu);
        if (p.id == "receiver") sawPeer = true;
        cv.notify_all();
    };
    scb.onTransferFinished = [&](const TransferInfo&, const TransferResult& r) {
        std::lock_guard<std::mutex> l(mu);
        senderDone = true;
        senderStatus = r.status;
        senderMsg = r.message;
        cv.notify_all();
    };

    Engine receiver({"receiver", "Receiver", "macos", dst.u8string(), port}, rcb);
    receiverPtr = &receiver;
    Engine sender({"sender", "Sender", "windows", "", port}, scb);
    std::string err;
    CHECK(receiver.start(&err));
    CHECK(sender.start(&err));

    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(5), [&] { return sawPeer; });
    }
    CHECK(sawPeer);
    if (!sawPeer) return;

    std::string id = sender.sendFiles("receiver", {(src / "big.bin").u8string(), (src / "Album").u8string()}, &err);
    CHECK(!id.empty());
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(30), [&] { return senderDone && receiverDone; });
    }
    if (senderStatus != TransferStatus::Completed) std::fprintf(stderr, "sender: %s\n", senderMsg.c_str());
    if (receiverStatus != TransferStatus::Completed) std::fprintf(stderr, "receiver: %s\n", receiverMsg.c_str());
    CHECK(senderStatus == TransferStatus::Completed);
    CHECK(receiverStatus == TransferStatus::Completed);
    CHECK(readAll(dst / "big (1).bin") == big);
    CHECK(readAll(dst / "big.bin") == "existing");
    CHECK(readAll(dst / "Album" / "a.txt") == "hello");
    CHECK(readAll(dst / "Album" / "sub" / "b.txt") == "world");

#ifndef _WIN32
    // Gửi lần 2 từ file descriptor (đường Android content://), đặt tên khác.
    {
        std::lock_guard<std::mutex> l(mu);
        senderDone = receiverDone = false;
    }
    int fd = ::open((src / "big.bin").c_str(), O_RDONLY);
    CHECK(fd >= 0);
    std::vector<FileEntry> entries{fileEntryFromFd(fd, "../tu fd.bin", big.size())};
    CHECK(!sender.sendEntries("receiver", std::move(entries), &err).empty());
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(30), [&] { return senderDone && receiverDone; });
    }
    CHECK(senderStatus == TransferStatus::Completed);
    CHECK(readAll(dst / "tu fd.bin") == big);
#endif

    sender.stop();
    receiver.stop();
    std::error_code ec;
    fs::remove_all(root, ec);
}

// Tạm ẩn: máy khác không thấy, vẫn thấy máy khác, tự động từ chối yêu cầu gửi file.
static void testHidden() {
    const uint16_t port = 45998;
    std::mutex mu;
    std::condition_variable cv;
    bool seen = false;
    bool incomingAsked = false;

    EngineCallbacks scb;
    scb.onPeerUpdated = [&](const Peer& p) {
        std::lock_guard<std::mutex> l(mu);
        if (p.id == "hid-recv") seen = true;
        cv.notify_all();
    };
    scb.onPeerLost = [&](const std::string& id) {
        std::lock_guard<std::mutex> l(mu);
        if (id == "hid-recv") seen = false;
        cv.notify_all();
    };
    EngineCallbacks rcb;
    rcb.onIncomingRequest = [&](const TransferInfo&) { incomingAsked = true; };

    Engine receiver({"hid-recv", "Hidden", "macos", "", port, false}, rcb);
    Engine sender({"hid-send", "Seeker", "windows", "", port, false}, scb);
    std::string err;
    CHECK(receiver.start(&err) && sender.start(&err));
    auto waitSeen = [&](bool want) {
        std::unique_lock<std::mutex> l(mu);
        return cv.wait_for(l, std::chrono::seconds(5), [&] { return seen == want; });
    };

    CHECK(waitSeen(true));
    receiver.setHidden(true);
    CHECK(receiver.isHidden());
    CHECK(waitSeen(false));  // nhận BYE => bị xoá khỏi danh sách
    sender.discover();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(!seen);  // không trả lời DISCOVER khi đang ẩn
    bool receiverSeesSender = false;
    for (const auto& p : receiver.peers()) receiverSeesSender |= p.id == "hid-send";
    CHECK(receiverSeesSender);  // máy đang ẩn vẫn thấy máy khác

    // Gửi thẳng gói SEND_REQUEST tới máy đang ẩn => phải nhận lại từ chối, reason=hidden.
    net::Socket sock = net::openUdp(0, false, &err);
    Message req(MessageType::SendRequest);
    req.set(field::kId, "raw-client");
    req.set(field::kName, "Raw");
    req.set(field::kRequest, "req-hidden-1");
    req.setUInt(field::kPort, net::localPort(sock));
    req.setInt(field::kCount, 1);
    const std::string data = req.encode();
    net::sendTo(sock, "127.0.0.1", receiver.controlPort(), data.data(), data.size());
    std::vector<bool> ready;
    bool gotDecline = false;
    if (net::waitReadable({sock.fd()}, 3000, &ready) > 0) {
        char buf[kMaxDatagramSize];
        net::Endpoint from;
        int n = net::recvFrom(sock, buf, sizeof(buf), &from);
        auto resp = n > 0 ? Message::decode(std::string_view(buf, static_cast<size_t>(n))) : std::nullopt;
        gotDecline = resp && resp->type() == MessageType::SendResponse && resp->getInt(field::kAccept, 1) == 0 &&
                     resp->get(field::kReason) == "hidden" && resp->get(field::kRequest) == "req-hidden-1";
    }
    CHECK(gotDecline);
    CHECK(!incomingAsked);  // không làm phiền người dùng

    receiver.setHidden(false);
    CHECK(waitSeen(true));  // hiện lại => máy khác thấy ngay
    sender.stop();
    receiver.stop();
}

// Gửi 1 request HTTP thô tới máy chủ web, trả về (mã trạng thái, body).
static std::pair<int, std::string> httpRequest(uint16_t port, const std::string& method, const std::string& target,
                                               const std::string& body = {}) {
    std::string err;
    net::Socket s = net::connectTcp("127.0.0.1", port, 3000, &err);
    if (!s.valid()) return {0, err};
    net::setIoTimeout(s, 25000);
    std::string req = method + " " + target + " HTTP/1.1\r\nHost: test\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\n\r\n" + body;
    if (!net::sendAll(s, req.data(), req.size())) return {0, "send"};
    std::string resp;
    char buf[8192];
    int n;
    while ((n = net::recvSome(s, buf, sizeof(buf))) > 0) resp.append(buf, static_cast<size_t>(n));
    if (resp.size() < 12) return {0, resp};
    int code = std::atoi(resp.substr(9, 3).c_str());
    size_t p = resp.find("\r\n\r\n");
    return {code, p == std::string::npos ? std::string() : resp.substr(p + 4)};
}

static std::string jsonField(const std::string& json, const std::string& key) {
    std::string k = "\"" + key + "\":\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    return json.substr(p, json.find('"', p) - p);
}

static void testWebServer() {
    const fs::path dst = fs::temp_directory_path() / ("filedrop_web_" + randomHex(4));
    fs::create_directories(dst);

    std::mutex mu;
    std::condition_variable cv;
    std::vector<WebClient> events;
    std::vector<TransferResult> results;
    std::vector<TransferInfo> finishedInfo;
    uint64_t lastProgress = 0;

    EngineCallbacks cb;
    cb.onWebClientChanged = [&](const WebClient& c) {
        std::lock_guard<std::mutex> l(mu);
        events.push_back(c);
        cv.notify_all();
    };
    cb.onProgress = [&](const TransferInfo&, uint64_t done, uint64_t) {
        std::lock_guard<std::mutex> l(mu);
        lastProgress = done;
    };
    cb.onTransferFinished = [&](const TransferInfo& i, const TransferResult& r) {
        std::lock_guard<std::mutex> l(mu);
        results.push_back(r);
        finishedInfo.push_back(i);
        cv.notify_all();
    };
    EngineConfig cfg;
    cfg.deviceName = "Máy chủ";
    cfg.downloadDir = dst.u8string();
    Engine engine(cfg, cb);

    std::string err;
    CHECK(engine.startWebServer(&err));
    CHECK(engine.isWebServerRunning());
    const std::string url = engine.webServerUrl();
    CHECK(url.rfind("http://", 0) == 0);
    const uint16_t port = static_cast<uint16_t>(std::atoi(url.substr(url.rfind(':') + 1).c_str()));
    CHECK(port != 0);

    auto page = httpRequest(port, "GET", "/");
    CHECK(page.first == 200 && page.second.find("FileDrop") != std::string::npos);
    auto info = httpRequest(port, "GET", "/api/info");
    CHECK(info.first == 200 && jsonField(info.second, "host") == "Máy chủ");
    CHECK(info.second.find("\"apps\":[]") != std::string::npos);

    // Bản cài cho máy khách tải về: file thiếu bị bỏ qua, file có thì tải được không cần duyệt.
    {
        const fs::path pkg = dst / "pkg.bin";
        std::string data(300000, '\0');
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 7);
        { std::ofstream(pkg, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size())); }
        engine.setWebDownloads({{"macos", pkg.u8string(), "FileDrop-macOS.zip"},
                                {"windows", (dst / "missing.exe").u8string(), "FileDrop.exe"}});
        auto info2 = httpRequest(port, "GET", "/api/info");
        CHECK(info2.second.find("\"platform\":\"macos\",\"name\":\"FileDrop-macOS.zip\",\"size\":300000") !=
              std::string::npos);
        CHECK(info2.second.find("windows") == std::string::npos);
        auto dl = httpRequest(port, "GET", "/download/macos");
        CHECK(dl.first == 200 && dl.second == data);
        CHECK(httpRequest(port, "GET", "/download/windows").first == 404);
        CHECK(httpRequest(port, "GET", "/download/linux").first == 404);
        // Thư mục bản cài: nhận theo đuôi file; bản khai báo trước được ưu tiên.
        const fs::path pkgDir = dst / "packages";
        fs::create_directories(pkgDir);
        { std::ofstream(pkgDir / "FileDrop.apk") << "apk"; }
        { std::ofstream(pkgDir / "FileDrop-Windows.zip") << "winzip"; }
        { std::ofstream(pkgDir / "FileDrop-macOS.zip") << "other mac"; }
        { std::ofstream(pkgDir / "notes.txt") << "x"; }
        auto scanned = scanWebDownloads(pkgDir.u8string());
        CHECK(scanned.size() == 3);
        engine.setWebDownloads({{"macos", pkg.u8string(), "FileDrop-macOS.zip"}}, {pkgDir.u8string()});
        auto info3 = httpRequest(port, "GET", "/api/info");
        CHECK(info3.second.find("\"size\":300000") != std::string::npos);
        CHECK(info3.second.find("\"platform\":\"android\",\"name\":\"FileDrop.apk\",\"size\":3") != std::string::npos);
        CHECK(httpRequest(port, "GET", "/download/windows").second == "winzip");
        CHECK(httpRequest(port, "GET", "/download/macos").second == data);
        fs::remove_all(pkgDir);
        fs::remove(pkg);
        engine.setWebDownloads({});
    }

    // Chưa được chấp nhận thì không gửi được.
    auto hello = httpRequest(port, "POST", "/api/hello", "Laptop của An");
    const std::string token = jsonField(hello.second, "session");
    const std::string clientId = jsonField(hello.second, "client");
    CHECK(hello.first == 200 && token.size() == 32);
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(3), [&] { return !events.empty(); });
        CHECK(!events.empty() && events[0].state == WebClientState::Pending && events[0].name == "Laptop của An" &&
              events[0].ip == "127.0.0.1");
    }
    CHECK(httpRequest(port, "POST", "/api/batch?s=" + token + "&count=1&total=5").first == 403);
    CHECK(engine.webClients().size() == 1);

    // Long-poll: trả về ngay khi máy chủ đồng ý.
    std::thread approver([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        engine.respondWebClient(clientId, true);
    });
    auto t0 = std::chrono::steady_clock::now();
    auto status = httpRequest(port, "GET", "/api/status?s=" + token + "&c=" + clientId + "&known=pending");
    approver.join();
    CHECK(status.first == 200 && jsonField(status.second, "state") == "accepted");
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));

    // Token sai => 403.
    CHECK(httpRequest(port, "POST", "/api/batch?s=deadbeef&count=1&total=5").first == 403);

    // Batch 2 file, 1 file nằm trong thư mục; tên trùng file có sẵn => đổi tên.
    std::ofstream(dst / "a.txt") << "old";
    std::string big(700 * 1024 + 3, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    const uint64_t total = 5 + big.size();
    auto batch = httpRequest(port, "POST", "/api/batch?s=" + token + "&count=2&total=" + std::to_string(total));
    const std::string batchId = jsonField(batch.second, "batch");
    CHECK(batch.first == 200 && !batchId.empty());
    auto up1 = httpRequest(port, "PUT", "/api/upload?s=" + token + "&b=" + batchId + "&path=a.txt", "hello");
    CHECK(up1.first == 200 && up1.second.find("\"done\":false") != std::string::npos);
    auto up2 = httpRequest(port, "PUT", "/api/upload?s=" + token + "&b=" + batchId +
                                            "&path=%E1%BA%A2nh%2F..%2F..%2Fbig.bin", big);  // "Ảnh/../../big.bin"
    CHECK(up2.first == 200 && up2.second.find("\"done\":true") != std::string::npos);
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(3), [&] { return !results.empty(); });
        CHECK(results.size() == 1 && results[0].status == TransferStatus::Completed);
        CHECK(results.size() == 1 && results[0].savedPaths.size() == 2);
        CHECK(finishedInfo.size() == 1 && finishedInfo[0].direction == Direction::Receive &&
              finishedInfo[0].peer.name == "Laptop của An" && finishedInfo[0].fileCount == 2);
        CHECK(lastProgress == total);
    }
    auto readAll = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    CHECK(readAll(dst / "a.txt") == "old");
    CHECK(readAll(dst / "a (1).txt") == "hello");
    CHECK(readAll(dst / fs::u8path("Ảnh") / "big.bin") == big);  // ".." bị loại bỏ, không thoát khỏi thư mục

    // Kết nối vẫn mở sau khi gửi xong: gửi tiếp lượt thứ 2 được.
    auto batch2 = httpRequest(port, "POST", "/api/batch?s=" + token + "&count=1&total=3&name=c.txt");
    const std::string batch2Id = jsonField(batch2.second, "batch");
    CHECK(httpRequest(port, "PUT", "/api/upload?s=" + token + "&b=" + batch2Id + "&path=c.txt", "abc").first == 200);
    CHECK(readAll(dst / "c.txt") == "abc");
    // Vượt quá tổng đã khai báo => từ chối.
    auto batch3 = httpRequest(port, "POST", "/api/batch?s=" + token + "&count=1&total=2");
    const std::string batch3Id = jsonField(batch3.second, "batch");
    CHECK(httpRequest(port, "PUT", "/api/upload?s=" + token + "&b=" + batch3Id + "&path=d.txt", "toolong").first == 400);

    // Máy chủ ngắt máy khách => trang nhận "closed", token hết hiệu lực.
    engine.disconnectWebClient(clientId);
    auto closed = httpRequest(port, "GET", "/api/status?s=" + token + "&c=" + clientId);
    CHECK(jsonField(closed.second, "state") == "closed");
    CHECK(httpRequest(port, "POST", "/api/batch?s=" + token + "&count=1&total=1").first == 403);
    CHECK(engine.webClients().empty());

    // Từ chối.
    auto hello2 = httpRequest(port, "POST", "/api/hello", "Điện thoại");
    const std::string token2 = jsonField(hello2.second, "session");
    const std::string client2 = jsonField(hello2.second, "client");
    CHECK(engine.respondWebClient(client2, false));
    CHECK(!engine.respondWebClient(client2, true));
    CHECK(jsonField(httpRequest(port, "GET", "/api/status?s=" + token2 + "&c=" + client2).second, "state") ==
          "declined");

    // Tắt máy chủ trong khi 1 trang đang long-poll => request trả về "closed", cổng đóng.
    auto hello3 = httpRequest(port, "POST", "/api/hello", "Máy 3");
    const std::string token3 = jsonField(hello3.second, "session");
    const std::string client3 = jsonField(hello3.second, "client");
    std::string pollState;
    std::thread poller([&] {
        pollState = jsonField(
            httpRequest(port, "GET", "/api/status?s=" + token3 + "&c=" + client3 + "&known=pending").second, "state");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto stopStart = std::chrono::steady_clock::now();
    engine.stopWebServer();
    poller.join();
    CHECK(std::chrono::steady_clock::now() - stopStart < std::chrono::seconds(3));
    CHECK(pollState == "closed" || pollState.empty());
    CHECK(!engine.isWebServerRunning());
    CHECK(httpRequest(port, "GET", "/").first == 0);

    std::error_code ec;
    fs::remove_all(dst, ec);
}

int main() {
    testProtocol();
    testEndToEnd();
    testHidden();
    testWebServer();
    if (gFailures == 0) std::printf("Tất cả kiểm thử đều đạt.\n");
    return gFailures == 0 ? 0 : 1;
}
