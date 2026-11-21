#include "Settings.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <vector>

namespace filedrop::win {

namespace {
constexpr wchar_t kSettingsKey[] = L"Software\\FileDrop";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"FileDrop";
constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

std::wstring readString(HKEY root, const wchar_t* key, const wchar_t* name, bool* found) {
    if (found) *found = false;
    DWORD size = 0;
    if (::RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) return {};
    value.resize(wcsnlen(value.c_str(), value.size()));
    if (found) *found = true;
    return value;
}

bool writeString(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    HKEY h = nullptr;
    if (::RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r = ::RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

bool readDword(const wchar_t* key, const wchar_t* name, DWORD* out) {
    DWORD size = sizeof(DWORD);
    return ::RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, out, &size) == ERROR_SUCCESS;
}
}  // namespace

std::wstring readSetting(const wchar_t* name, const std::wstring& fallback) {
    bool found = false;
    std::wstring v = readString(HKEY_CURRENT_USER, kSettingsKey, name, &found);
    return found ? v : fallback;
}

void writeSetting(const wchar_t* name, const std::wstring& value) {
    writeString(HKEY_CURRENT_USER, kSettingsKey, name, value);
}

std::wstring executablePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

bool isAutostartEnabled() {
    bool found = false;
    readString(HKEY_CURRENT_USER, kRunKey, kRunValue, &found);
    return found;
}

bool setAutostart(bool enabled) {
    if (enabled) {
        return writeString(HKEY_CURRENT_USER, kRunKey, kRunValue, L"\"" + executablePath() + L"\" --autostart");
    }
    HKEY h = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return true;
    LONG r = ::RegDeleteValueW(h, kRunValue);
    ::RegCloseKey(h);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

std::wstring downloadsFolder() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &path)) && path) {
        result = path;
    }
    ::CoTaskMemFree(path);
    return result;
}

bool appsUseDarkTheme() {
    DWORD light = 1;
    return readDword(kPersonalizeKey, L"AppsUseLightTheme", &light) && light == 0;
}

bool taskbarUsesDarkTheme() {
    DWORD light = 0;
    if (!readDword(kPersonalizeKey, L"SystemUsesLightTheme", &light)) return true;
    return light == 0;
}

}  // namespace filedrop::win
