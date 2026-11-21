// FileDrop — tiện ích dùng chung.
#pragma once

#include <cstdint>
#include <string>

namespace filedrop {

// Chuỗi ngẫu nhiên gồm [A-Za-z0-9], dùng cho mật khẩu phiên truyền.
std::string randomToken(size_t length);
// Chuỗi hex ngẫu nhiên (bytes * 2 ký tự), dùng cho ID.
std::string randomHex(size_t bytes);
// So sánh thời gian hằng (chống timing attack).
bool constantTimeEquals(const std::string& a, const std::string& b);

// "macos" | "windows" | "linux" | "ios" | "android"
std::string currentOs();
std::string defaultDeviceName();
std::string defaultDownloadDir();

#ifdef _WIN32
std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);
#endif

}  // namespace filedrop
