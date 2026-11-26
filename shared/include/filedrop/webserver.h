// FileDrop — "Máy chủ nhận file": HTTP server nhỏ trên cổng ngẫu nhiên, phục vụ sẵn một trang web
// để máy bất kỳ trong mạng (không cần cài app) mở trình duyệt và gửi file tới máy này.
//
// Luồng:
//   1. Máy khách mở http://<ip>:<port>/ → trang hỏi tên → POST /api/hello → máy chủ hỏi người dùng
//      (callback onClientChanged với trạng thái Pending) → UI gọi respond().
//   2. Trang chờ bằng long-poll GET /api/status. Được chấp nhận → hiện vùng thả file.
//   3. Mỗi lần thả: POST /api/batch (số file, tổng dung lượng) rồi PUT /api/upload cho từng file.
//      Mỗi batch là một phiên nhận (TransferInfo, chiều Receive) → dùng chung progress/thông báo với UDP/TCP.
//   4. Kết nối giữ nguyên sau khi gửi xong; chỉ kết thúc khi người dùng ngắt máy khách hoặc tắt máy chủ.
// Ngoài ra trang web gợi ý tải bản cài FileDrop hợp với HĐH máy khách (GET /download/<platform>, không cần duyệt).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "filedrop/engine.h"

namespace filedrop {

struct WebServerCallbacks {
    std::function<void(const WebClient&)> onClientChanged;
    std::function<void(const TransferInfo&, TransferPhase)> onTransferPhase;
    std::function<void(const TransferInfo&, uint64_t done, uint64_t total)> onProgress;
    std::function<void(const TransferInfo&, const TransferResult&)> onTransferFinished;
};

// Quét (không đệ quy) các bản cài trong thư mục, nhận theo đuôi/tên file:
// .apk → android, .exe/.msi → windows, .dmg → macos, .AppImage/.deb → linux,
// .zip có "mac"/"win"/"linux" trong tên → nền tảng tương ứng.
std::vector<WebDownload> scanWebDownloads(const std::string& dir);

class WebServer {
public:
    explicit WebServer(WebServerCallbacks callbacks);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Mở cổng TCP ngẫu nhiên. hostName hiện trên trang web; file nhận lưu vào downloadDir.
    bool start(const std::string& hostName, const std::string& downloadDir, std::string* error = nullptr);
    // Ngắt mọi máy khách, huỷ các phiên đang nhận dở, đóng cổng.
    void stop();
    bool isRunning() const;

    uint16_t port() const;
    // "http://<ip LAN>:<port>/" — rỗng nếu chưa chạy.
    std::string url() const;

    // Trả lời yêu cầu kết nối của máy khách. false nếu không còn yêu cầu đó.
    bool respond(const std::string& clientId, bool accept);
    // Ngắt một máy khách đã kết nối (huỷ luôn phiên đang nhận dở của máy đó).
    void disconnect(const std::string& clientId);
    // Huỷ một phiên nhận. false nếu transferId không thuộc máy chủ web.
    bool cancel(const std::string& transferId);
    std::vector<WebClient> clients() const;

    void setHostName(const std::string& name);
    void setDownloadDir(const std::string& dir);
    // Bản cài FileDrop cho máy khách tải (GET /download/<platform>); liệt kê trong /api/info.
    // Mỗi nền tảng lấy mục đầu tiên: danh sách downloads trước, rồi file quét được trong packageDirs.
    void setDownloads(std::vector<WebDownload> downloads, std::vector<std::string> packageDirs = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace filedrop
