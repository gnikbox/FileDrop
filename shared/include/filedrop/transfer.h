// FileDrop — truyền file qua TCP.
//
// Luồng trên kết nối TCP:
//   Gửi -> Nhận : "FDTX" + mật khẩu (16 byte)
//   Nhận -> Gửi : 'A' (chấp nhận, bắt đầu nhận) | 'R' (sai mật khẩu)
//   Gửi -> Nhận : u32 số file, u64 tổng byte
//                 lặp: u32 độ dài đường dẫn, đường dẫn tương đối UTF-8 ('/'), u64 kích thước, dữ liệu
//   Nhận -> Gửi : 'D' (đã nhận đủ)
// Số nguyên dạng big-endian.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "filedrop/net.h"

namespace filedrop {

inline constexpr size_t kPasswordLength = 16;

struct FileEntry {
    std::string absPath;  // đường dẫn tuyệt đối trên máy gửi (UTF-8)
    std::string relPath;  // đường dẫn tương đối gửi sang máy nhận, phân tách '/'
    uint64_t size = 0;
    // (POSIX) Đọc dữ liệu từ file descriptor thay cho absPath — dùng cho content:// trên Android.
    // fdOwner giữ quyền sở hữu: fd tự đóng khi FileEntry cuối cùng tham chiếu tới nó bị huỷ.
    int fd = -1;
    std::shared_ptr<void> fdOwner;
};

#ifndef _WIN32
// Tạo FileEntry từ một file descriptor của file thường. FileEntry nhận quyền sở hữu fd.
FileEntry fileEntryFromFd(int fd, const std::string& name, uint64_t size);
#endif

struct TransferOutcome {
    bool ok = false;
    std::string error;
    uint64_t bytes = 0;
    std::vector<std::string> savedPaths;  // (máy nhận) các file/thư mục cấp cao nhất đã lưu
};

// Chọn nơi lưu an toàn trong destDir cho đường dẫn tương đối nhận từ mạng: bỏ "..", ký tự không hợp lệ,
// trùng tên thì thêm " (1)"; các file chung thư mục gốc (vd. "Ảnh/a.jpg", "Ảnh/b.jpg") được gom vào cùng 1 thư mục.
class SaveLocator {
public:
    explicit SaveLocator(std::string destDir) : dest_(std::move(destDir)) {}
    // Đường dẫn tuyệt đối (UTF-8) để ghi file; thư mục cha đã được tạo sẵn.
    std::string resolve(const std::string& relPath);
    // Các file/thư mục cấp cao nhất đã tạo, theo thứ tự.
    const std::vector<std::string>& topLevelPaths() const { return top_; }

private:
    std::string dest_;
    std::map<std::string, std::string> dirs_;  // tên thư mục gốc gửi sang -> thư mục đã tạo
    std::vector<std::string> top_;
};

using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

// Mở rộng danh sách đường dẫn (file hoặc thư mục) thành danh sách file cần gửi.
std::vector<FileEntry> collectFiles(const std::vector<std::string>& paths);

TransferOutcome runSender(const std::string& ip, uint16_t port, const std::string& password,
                          const std::vector<FileEntry>& files, const ProgressFn& progress,
                          const std::atomic<bool>& cancel);

// Chờ máy gửi kết nối tới listener, kiểm tra mật khẩu rồi nhận file vào destDir.
TransferOutcome runReceiver(net::Socket& listener, const std::string& password, const std::string& destDir,
                            int acceptTimeoutMs, const std::function<void()>& onAuthenticated,
                            const ProgressFn& progress, const std::atomic<bool>& cancel);

}  // namespace filedrop
