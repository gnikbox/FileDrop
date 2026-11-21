// FileDrop (Windows) — cửa sổ popup bo tròn, đổ bóng, hiện cạnh icon khay hệ thống.
// Bố cục đồng nhất với bản macOS (macOS/FileDrop/Sources/ContentView.swift).
#pragma once

#include <windows.h>
#include <ole2.h>

#include <string>
#include <vector>

#include "Drawing.h"

namespace filedrop::win {

class App;
class DropTarget;

class PopupWindow {
public:
    explicit PopupWindow(App& app);
    ~PopupWindow();

    bool create(HINSTANCE instance);
    void show();
    void hide();
    void toggle();
    bool visible() const;
    void invalidate();  // trạng thái App thay đổi
    void themeChanged();

    // Dùng bởi DropTarget (toạ độ màn hình).
    DWORD dragEffect(POINTL screenPt);
    void setDropHover(bool hover);
    void dropFiles(const std::vector<std::wstring>& paths, POINTL screenPt);

private:
    enum class Hit { None, Refresh, Tile, Autostart, Quit, Cancel };

    struct Layout {
        float width = 0, height = 0;
        Gdiplus::RectF header, refresh, section, grid, drop, status, divider, autostart, quit, cancel;
        std::vector<Gdiplus::RectF> tiles;  // toạ độ trong vùng lưới (chưa trừ cuộn)
        float gridContentHeight = 0;
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);

    void computeLayout();
    void reposition();
    void paint(HDC hdc);
    Hit hitTest(POINT pt, int* tileIndex) const;
    bool pointInDropZone(POINTL screenPt) const;
    bool animating() const;

    void drawText(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::RectF& r, float sizeDip, int style,
                  const Gdiplus::Color& color, Gdiplus::StringAlignment h = Gdiplus::StringAlignmentNear,
                  Gdiplus::StringAlignment v = Gdiplus::StringAlignmentCenter, bool wrap = false,
                  const wchar_t* family = nullptr);
    void drawGlyph(Gdiplus::Graphics& g, const wchar_t* glyph, const Gdiplus::RectF& r, float sizeDip,
                   const Gdiplus::Color& color);
    void drawSpinner(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float width, const Gdiplus::Color& color);
    void drawProgressRing(Gdiplus::Graphics& g, const Gdiplus::RectF& r, double progress, bool indeterminate);
    void drawPeerGrid(Gdiplus::Graphics& g);
    void drawDropZone(Gdiplus::Graphics& g);
    void drawFooter(Gdiplus::Graphics& g);

    float px(float dip) const { return dip * scale_; }

    App& app_;
    HWND hwnd_ = nullptr;
    DropTarget* dropTarget_ = nullptr;
    Theme theme_;
    Layout layout_;
    float scale_ = 1.0f;
    float scrollOffset_ = 0;
    bool dropHover_ = false;
    bool trackingMouse_ = false;
    Hit hover_ = Hit::None;
    int hoverTile_ = -1;
};

}  // namespace filedrop::win
