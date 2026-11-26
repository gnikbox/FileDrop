// FileDrop (Windows) — màu sắc, font, icon và các hàm vẽ GDI+ dùng chung.
#pragma once

#include <windows.h>
#include <objidl.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace filedrop::win {

// Bảng màu đồng bộ với bản macOS (accent = systemBlue của macOS).
struct Theme {
    Gdiplus::Color background;
    Gdiplus::Color text;
    Gdiplus::Color secondaryText;
    Gdiplus::Color tile;
    Gdiplus::Color tileHover;
    Gdiplus::Color button;
    Gdiplus::Color dashed;
    Gdiplus::Color divider;
    Gdiplus::Color accent;
    Gdiplus::Color accentSoft;
    Gdiplus::Color success;
    Gdiplus::Color warning;
    bool dark = false;

    static Theme current();
};

// Glyph trong font Segoe Fluent Icons / Segoe MDL2 Assets.
namespace glyph {
inline constexpr const wchar_t* kApp = L"";      // Wifi
inline constexpr const wchar_t* kRefresh = L"";
inline constexpr const wchar_t* kLaptop = L"";   // Mac
inline constexpr const wchar_t* kPc = L"";       // PC Windows
inline constexpr const wchar_t* kDesktop = L"";
inline constexpr const wchar_t* kPhone = L"";
inline constexpr const wchar_t* kDrop = L"";     // Download
inline constexpr const wchar_t* kCheck = L"";
inline constexpr const wchar_t* kSuccess = L"";  // Completed
inline constexpr const wchar_t* kError = L"";
inline constexpr const wchar_t* kGlobe = L"\uE774";   // Globe — máy chủ web
inline constexpr const wchar_t* kCopy = L"\uE8C8";
inline constexpr const wchar_t* kOpenLink = L"\uE8A7";  // OpenInNewWindow
}  // namespace glyph

const wchar_t* iconFontFamily();
const wchar_t* textFontFamily();

void addRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius);

// Icon khay hệ thống: progress < 0 => icon thường; ngược lại vòng tiến trình 0..1.
HICON createTrayIcon(int sizePx, bool darkTaskbar, double progress);

// Tạo bản sao icon với độ trong suốt nhân thêm alphaFactor (0..1) — dùng khi đang tạm ẩn.
HICON createDimmedIcon(HICON source, double alphaFactor);

}  // namespace filedrop::win
