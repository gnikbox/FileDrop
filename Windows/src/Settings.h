// FileDrop (Windows) — lưu cài đặt trong registry + khởi động cùng Windows.
#pragma once

#include <string>

namespace filedrop::win {

// HKCU\Software\FileDrop
std::wstring readSetting(const wchar_t* name, const std::wstring& fallback = {});
void writeSetting(const wchar_t* name, const std::wstring& value);

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run
bool isAutostartEnabled();
bool setAutostart(bool enabled);

std::wstring downloadsFolder();
std::wstring executablePath();
bool appsUseDarkTheme();
bool taskbarUsesDarkTheme();

}  // namespace filedrop::win
