// FileDrop — Engine: điều phối khám phá thiết bị (UDP) và truyền file (TCP).
//
// Mọi callback được gọi từ luồng nền của Engine; tầng UI phải tự chuyển về main thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "filedrop/protocol.h"
#include "filedrop/transfer.h"

namespace filedrop {

struct Peer {
    std::string id;
    std::string name;
    std::string os;  // macos | windows | linux | ios | android
    std::string ip;
    uint16_t port = 0;  // port UDP điều khiển

    bool operator==(const Peer& o) const {
        return id == o.id && name == o.name && os == o.os && ip == o.ip && port == o.port;
    }
    bool operator!=(const Peer& o) const { return !(*this == o); }
};

enum class Direction { Send = 0, Receive = 1 };

enum class TransferPhase {
    WaitingForAccept = 0,  // (gửi) đã gửi yêu cầu, chờ máy nhận đồng ý
    Connecting = 1,        // đang thiết lập kết nối TCP / xác thực mật khẩu
    Transferring = 2,      // đang truyền dữ liệu
};

enum class TransferStatus { Completed = 0, Declined = 1, Failed = 2, Cancelled = 3, TimedOut = 4 };

struct TransferInfo {
    std::string id;  // ID yêu cầu, giống nhau ở cả 2 máy
    Direction direction = Direction::Send;
    Peer peer;  // máy bên kia
    int fileCount = 0;
    std::string fileName;  // tên file khi chỉ có 1 file
    uint64_t totalBytes = 0;
};

struct TransferResult {
    TransferStatus status = TransferStatus::Failed;
    std::string message;
    std::string saveDir;                  // (nhận) thư mục lưu
    std::vector<std::string> savedPaths;  // (nhận) file/thư mục đã lưu
};

struct EngineCallbacks {
    std::function<void(const Peer&)> onPeerUpdated;
    std::function<void(const std::string& peerId)> onPeerLost;
    // Có yêu cầu nhận file — UI hỏi người dùng rồi gọi Engine::respond().
    std::function<void(const TransferInfo&)> onIncomingRequest;
    std::function<void(const TransferInfo&, TransferPhase)> onTransferPhase;
    std::function<void(const TransferInfo&, uint64_t done, uint64_t total)> onProgress;
    std::function<void(const TransferInfo&, const TransferResult&)> onTransferFinished;
};

struct EngineConfig {
    std::string deviceId;    // ID cố định của máy (rỗng => tạo ngẫu nhiên)
    std::string deviceName;  // rỗng => tên máy hệ thống
    std::string os;          // rỗng => tự nhận diện
    std::string downloadDir; // rỗng => ~/Downloads
    uint16_t discoveryPort = kDiscoveryPort;
};

class Engine {
public:
    Engine(EngineConfig config, EngineCallbacks callbacks);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start(std::string* error = nullptr);
    void stop();
    bool isRunning() const;

    // Broadcast gói hỏi danh tính tới toàn mạng LAN.
    void discover();
    std::vector<Peer> peers() const;

    // Gửi yêu cầu truyền file tới peerId. Trả về ID phiên (rỗng nếu lỗi).
    std::string sendFiles(const std::string& peerId, const std::vector<std::string>& paths,
                          std::string* error = nullptr);
    // Như sendFiles nhưng với danh sách file đã chuẩn bị sẵn (vd. từ file descriptor trên Android).
    std::string sendEntries(const std::string& peerId, std::vector<FileEntry> files, std::string* error = nullptr);
    // Trả lời yêu cầu nhận file. Trả về false nếu yêu cầu không còn tồn tại.
    bool respond(const std::string& transferId, bool accept);
    void cancel(const std::string& transferId);

    void setDeviceName(const std::string& name);
    void setDownloadDir(const std::string& dir);
    std::string deviceId() const;
    std::string deviceName() const;
    uint16_t controlPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace filedrop
