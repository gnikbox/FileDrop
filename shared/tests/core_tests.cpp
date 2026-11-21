// Kiểm thử lõi: mã hoá giao thức + truyền file end-to-end giữa 2 Engine trên cùng máy.
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include "filedrop/protocol.h"
#include "filedrop/util.h"

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

int main() {
    testProtocol();
    testEndToEnd();
    if (gFailures == 0) std::printf("Tất cả kiểm thử đều đạt.\n");
    return gFailures == 0 ? 0 : 1;
}
