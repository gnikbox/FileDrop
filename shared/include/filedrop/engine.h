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

// Máy khách của "Máy chủ nhận file" (trình duyệt web) — xem webserver.h.
enum class WebClientState {
    Pending = 0,       // đang chờ người dùng máy chủ đồng ý
    Connected = 1,     // đã được chấp nhận, gửi file được
    Declined = 2,      // bị từ chối
    Disconnected = 3,  // đã ngắt (máy chủ ngắt, máy khách tự ngắt, hoặc máy chủ tắt)
};

struct WebClient {
    std::string id;
    std::string name;  // tên máy khách tự nhập trên trang web
    std::string ip;
    WebClientState state = WebClientState::Pending;
};

// Bản cài FileDrop mà máy chủ web cho máy khách tải về (trang web chọn bản hợp với HĐH của máy khách).
struct WebDownload {
    std::string platform;  // "macos" | "windows" | "android" | "ios" | "linux"
    std::string path;      // file trên máy này (UTF-8)
    std::string fileName;  // tên gợi ý khi tải, vd. "FileDrop-macOS.zip"
};

struct EngineCallbacks {
    std::function<void(const Peer&)> onPeerUpdated;
    std::function<void(const std::string& peerId)> onPeerLost;
    // Có yêu cầu nhận file — UI hỏi người dùng rồi gọi Engine::respond().
    std::function<void(const TransferInfo&)> onIncomingRequest;
    std::function<void(const TransferInfo&, TransferPhase)> onTransferPhase;
    std::function<void(const TransferInfo&, uint64_t done, uint64_t total)> onProgress;
    std::function<void(const TransferInfo&, const TransferResult&)> onTransferFinished;
    // Máy khách web xin kết nối (Pending) — UI hỏi người dùng rồi gọi respondWebClient().
    // Cũng được gọi khi máy khách được chấp nhận / bị từ chối / ngắt kết nối.
    // Các lượt gửi file từ trình duyệt đi qua onTransferPhase/onProgress/onTransferFinished (chiều Receive, peer.os = "web").
    std::function<void(const WebClient&)> onWebClientChanged;
};

struct EngineConfig {
    std::string deviceId;    // ID cố định của máy (rỗng => tạo ngẫu nhiên)
    std::string deviceName;  // rỗng => tên máy hệ thống
    std::string os;          // rỗng => tự nhận diện
    std::string downloadDir; // rỗng => ~/Downloads
    uint16_t discoveryPort = kDiscoveryPort;
    bool hidden = false;     // khởi động ở chế độ tạm ẩn
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

    // ---- Máy chủ nhận file qua trình duyệt (HTTP, cổng ngẫu nhiên) ----
    bool startWebServer(std::string* error = nullptr);
    // Ngắt mọi máy khách web và đóng cổng.
    void stopWebServer();
    bool isWebServerRunning() const;
    std::string webServerUrl() const;  // "http://192.168.1.5:51234/", rỗng nếu chưa bật
    bool respondWebClient(const std::string& clientId, bool accept);
    void disconnectWebClient(const std::string& clientId);
    std::vector<WebClient> webClients() const;  // đang chờ duyệt + đã kết nối
    // Các bản cài cho máy khách tải về qua GET /download/<platform> (không cần được duyệt). File thiếu thì bỏ qua.
    // packageDirs: thư mục chứa thêm bản cài cho nền tảng khác (quét mỗi lần có yêu cầu, xem scanWebDownloads).
    void setWebDownloads(std::vector<WebDownload> downloads, std::vector<std::string> packageDirs = {});

    void setDeviceName(const std::string& name);
    void setDownloadDir(const std::string& dir);

    // Tạm ẩn: máy khác không thấy máy này, mọi yêu cầu gửi file tới đều bị tự động từ chối.
    // Vẫn thấy máy khác và vẫn gửi file đi được.
    void setHidden(bool hidden);
    bool isHidden() const;
    std::string deviceId() const;
    std::string deviceName() const;
    uint16_t controlPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace filedrop
