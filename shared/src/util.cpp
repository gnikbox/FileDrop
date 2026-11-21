#include "filedrop/util.h"

#include <cstdlib>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace filedrop {

namespace {
std::random_device& rng() {
    thread_local std::random_device rd;
    return rd;
}
}  // namespace

std::string randomToken(size_t length) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<size_t> dist(0, sizeof(kAlphabet) - 2);
    std::string out;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) out.push_back(kAlphabet[dist(rng())]);
    return out;
}

std::string randomHex(size_t bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        int b = dist(rng());
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::string currentOs() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return "ios";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__ANDROID__)
    return "android";
#else
    return "linux";
#endif
}

#ifdef _WIN32
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}
#endif

std::string defaultDeviceName() {
#ifdef _WIN32
    wchar_t buf[256];
    DWORD size = 256;
    if (::GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &size)) return toUtf8(std::wstring(buf, size));
    return "Windows PC";
#else
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "Unknown";
    std::string name = buf;
    const std::string suffix = ".local";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.resize(name.size() - suffix.size());
    }
    return name;
#endif
}

std::string defaultDownloadDir() {
#ifdef _WIN32
    wchar_t* profile = nullptr;
    size_t len = 0;
    std::wstring dir;
    if (_wdupenv_s(&profile, &len, L"USERPROFILE") == 0 && profile) {
        dir = std::wstring(profile) + L"\\Downloads";
        free(profile);
    }
    return toUtf8(dir);
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Downloads";
#endif
}

}  // namespace filedrop
