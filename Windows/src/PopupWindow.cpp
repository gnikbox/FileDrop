#include "PopupWindow.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <cmath>

#include "App.h"
#include "DropData.h"
#include "resource.h"

using namespace Gdiplus;

namespace filedrop::win {

namespace {

constexpr wchar_t kPopupClass[] = L"FileDropPopup";
constexpr UINT_PTR kAnimTimer = 10;
constexpr float kWidthDip = 340;
constexpr float kPadDip = 16;
constexpr float kGapDip = 12;
constexpr float kTileHDip = 84;
constexpr float kTileGapDip = 8;

// DWM (Windows 11) — khai báo lại phòng khi SDK cũ chưa có.
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmCornerRound = 2;

const wchar_t* glyphForOs(const std::string& os) {
    if (os == "macos") return glyph::kLaptop;
    if (os == "windows") return glyph::kPc;
    if (os == "ios" || os == "android") return glyph::kPhone;
    return glyph::kDesktop;
}

}  // namespace

// --------------------------------------------------------------------------- DropTarget (OLE)

class DropTarget : public IDropTarget {
public:
    explicit DropTarget(PopupWindow* owner) : owner_(owner) {}
    virtual ~DropTarget() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ::InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = ::InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        // File thật, file ảo (Outlook...) hoặc ảnh (trình duyệt, Snipping Tool...).
        hasFiles_ = hasDroppableData(data);
        owner_->noteDragEntered();  // đang kéo vào popup => không tự đóng khi thả chuột
        return DragOver(0, pt, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        *effect = hasFiles_ ? owner_->dragEffect(pt) : DROPEFFECT_NONE;
        owner_->setDropHover(*effect != DROPEFFECT_NONE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        owner_->setDropHover(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        owner_->setDropHover(false);
        *effect = DROPEFFECT_NONE;
        if (!hasFiles_ || owner_->dragEffect(pt) == DROPEFFECT_NONE) return S_OK;

        std::vector<std::wstring> paths = readDroppedFiles(data);
        if (paths.empty()) return S_OK;
        *effect = DROPEFFECT_COPY;
        owner_->dropFiles(paths, pt);
        return S_OK;
    }

private:
    PopupWindow* owner_;
    LONG refs_ = 1;
    bool hasFiles_ = false;
};

// --------------------------------------------------------------------------- PopupWindow

PopupWindow::PopupWindow(App& app) : app_(app), theme_(Theme::current()) {}

PopupWindow::~PopupWindow() {
    if (hwnd_) {
        ::RevokeDragDrop(hwnd_);
        ::DestroyWindow(hwnd_);
    }
    if (dropTarget_) dropTarget_->Release();
}

bool PopupWindow::create(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DROPSHADOW | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PopupWindow::wndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"FileDrop", WS_POPUP, 0, 0, 340, 420,
                              nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    DWORD corner = kDwmCornerRound;  // bo tròn góc trên Windows 11
    ::DwmSetWindowAttribute(hwnd_, kDwmWindowCornerPreference, &corner, sizeof(corner));

    dropTarget_ = new DropTarget(this);
    ::RegisterDragDrop(hwnd_, dropTarget_);
    return true;
}

bool PopupWindow::visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }

void PopupWindow::show() {
    reposition();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    mouseWasDown_ = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (::GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    pressStartedInside_ = true;
    ::SetTimer(hwnd_, kAnimTimer, 33, nullptr);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PopupWindow::hide() {
    if (!visible()) return;
    ::KillTimer(hwnd_, kAnimTimer);
    ::ShowWindow(hwnd_, SW_HIDE);
    hiddenAt_ = ::GetTickCount64();
}

void PopupWindow::toggle() {
    if (visible()) {
        hide();
    } else if (::GetTickCount64() - hiddenAt_ > 400) {
        // Popup vừa tự đóng vì chính cú bấm vào icon khay này => không mở lại.
        show();
    }
}

bool PopupWindow::pointOverSelfOrTray(POINT pt) const {
    RECT r{};
    if (::GetWindowRect(hwnd_, &r) && ::PtInRect(&r, pt)) return true;
    NOTIFYICONIDENTIFIER id{sizeof(id)};
    id.hWnd = app_.messageWindow();
    id.uID = kTrayIconId;
    return SUCCEEDED(::Shell_NotifyIconGetRect(&id, &r)) && ::PtInRect(&r, pt);
}

// Popup có thể không được active (người dùng vừa bấm sang Explorer để kéo file), nên không dựa vào
// WM_ACTIVATE mà theo dõi trạng thái chuột toàn hệ thống:
//   - nhấn chuột ngoài popup/icon khay rồi thả ra mà KHÔNG kéo file vào popup => đóng;
//   - nhấn ở Explorer rồi kéo file vào popup => giữ nguyên để thả.
void PopupWindow::pollOutsideClick() {
    const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (::GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    POINT pt{};
    ::GetCursorPos(&pt);
    if (down && !mouseWasDown_) {
        pressStartedInside_ = pointOverSelfOrTray(pt);
        dragEnteredDuringPress_ = false;
    } else if (!down && mouseWasDown_) {
        RECT r{};
        bool releasedOverSelf = ::GetWindowRect(hwnd_, &r) && ::PtInRect(&r, pt);
        if (!pressStartedInside_ && !dragEnteredDuringPress_ && !releasedOverSelf) hide();
    }
    mouseWasDown_ = down;
}

void PopupWindow::invalidate() {
    if (!visible()) return;
    float oldHeight = layout_.height;
    computeLayout();
    if (std::fabs(oldHeight - layout_.height) > 0.5f) reposition();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PopupWindow::themeChanged() {
    theme_ = Theme::current();
    if (visible()) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

// Đặt popup cạnh icon khay, phía trên thanh taskbar (hoặc cạnh taskbar nếu taskbar nằm dọc).
void PopupWindow::reposition() {
    NOTIFYICONIDENTIFIER id{sizeof(id)};
    id.hWnd = app_.messageWindow();
    id.uID = kTrayIconId;
    RECT iconRect{};
    POINT anchor{};
    if (SUCCEEDED(::Shell_NotifyIconGetRect(&id, &iconRect))) {
        anchor = {(iconRect.left + iconRect.right) / 2, (iconRect.top + iconRect.bottom) / 2};
    } else {
        ::GetCursorPos(&anchor);
    }

    HMONITOR monitor = ::MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    ::GetMonitorInfoW(monitor, &mi);
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = 96;
    scale_ = dpiX / 96.0f;
    computeLayout();

    const RECT work = mi.rcWork, full = mi.rcMonitor;
    const int w = static_cast<int>(std::ceil(layout_.width));
    const int h = static_cast<int>(std::ceil(layout_.height));
    const int margin = static_cast<int>(px(12));
    auto clampInt = [](int v, int lo, int hi) { return hi < lo ? lo : std::min(std::max(v, lo), hi); };

    int x = clampInt(anchor.x - w / 2, work.left + margin, work.right - w - margin);
    int y = work.bottom - h - margin;
    if (work.top > full.top) {
        y = work.top + margin;  // taskbar ở trên
    } else if (work.left > full.left) {
        x = work.left + margin;  // taskbar bên trái
        y = clampInt(anchor.y - h / 2, work.top + margin, work.bottom - h - margin);
    } else if (work.right < full.right) {
        x = work.right - w - margin;  // taskbar bên phải
        y = clampInt(anchor.y - h / 2, work.top + margin, work.bottom - h - margin);
    }
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

void PopupWindow::computeLayout() {
    Layout L;
    const float pad = px(kPadDip), gap = px(kGapDip);
    L.width = px(kWidthDip);
    const float x = pad, cw = L.width - 2 * pad;
    float y = pad;

    L.header = RectF(x, y, cw, px(32));
    L.refresh = RectF(x + cw - px(28), y + px(2), px(28), px(28));
    y += px(32) + gap;

    L.section = RectF(x, y, cw, px(14));
    y += px(14) + gap;

    const size_t n = app_.peers().size();
    const float tileH = px(kTileHDip), tileGap = px(kTileGapDip);
    const float tileW = (cw - 2 * tileGap) / 3;
    for (size_t i = 0; i < n; ++i) {
        float col = static_cast<float>(i % 3), row = static_cast<float>(i / 3);
        L.tiles.emplace_back(x + col * (tileW + tileGap), row * (tileH + tileGap), tileW, tileH);
    }
    const size_t rows = (n + 2) / 3;
    L.gridContentHeight = rows ? rows * tileH + (rows - 1) * tileGap : 0;
    const float gridH = n == 0 ? px(92) : (n > 3 ? 2 * tileH + tileGap + px(16) : tileH);
    L.grid = RectF(x, y, cw, gridH);
    y += gridH + gap;

    L.drop = RectF(x, y, cw, px(130));
    y += px(130) + gap;

    L.status = RectF(x, y, cw, px(16));
    y += px(16) + gap;

    L.divider = RectF(x, y, cw, std::max(1.0f, px(1)));
    y += px(1) + gap;

    // Footer giống macOS: "Máy chủ nhận file" (+ khung địa chỉ khi bật), "Tạm ẩn",
    // rồi "Khởi động cùng Windows" + "Thoát".
    L.webToggle = RectF(x, y, px(260), px(24));
    y += px(24) + px(8);
    if (!app_.webServerUrl().empty()) {
        const float ip = px(10), iw = cw - 2 * ip;
        float py = y + ip;
        L.webOpen = RectF(x + cw - ip - px(22), py, px(22), px(22));
        L.webCopy = RectF(L.webOpen.X - px(28), py, px(22), px(22));
        L.webUrl = RectF(x + ip, py, L.webCopy.X - px(6) - (x + ip), px(22));
        py += px(22) + px(6);
        const auto& clients = app_.webClients();
        if (clients.empty()) {
            L.webHint = RectF(x + ip, py, iw, px(30));
            py += px(30);
        } else {
            for (size_t i = 0; i < clients.size(); ++i) {
                L.webRows.emplace_back(x + ip, py, iw, px(20));
                L.webDisconnect.emplace_back(x + ip + iw - px(36), py, px(36), px(20));
                py += px(20) + (i + 1 < clients.size() ? px(4) : 0);
            }
        }
        py += ip;
        L.webPanel = RectF(x, y, cw, py - y);
        y = py + px(8);
    }
    L.hiddenToggle = RectF(x, y, px(240), px(24));
    y += px(24) + px(8);
    L.autostart = RectF(x, y, px(220), px(24));
    L.quit = RectF(x + cw - px(64), y, px(64), px(24));
    y += px(24) + pad;

    L.height = y;
    L.cancel = layout_.cancel;
    layout_ = std::move(L);

    float maxScroll = std::max(0.0f, layout_.gridContentHeight - layout_.grid.Height);
    scrollOffset_ = std::min(std::max(scrollOffset_, 0.0f), maxScroll);
}

bool PopupWindow::animating() const {
    const UiTransfer* t = app_.currentTransfer();
    return (t && t->phase != TransferPhase::Transferring) || (app_.peers().empty() && app_.isSearching());
}

// --------------------------------------------------------------------------- Vẽ

void PopupWindow::drawText(Graphics& g, const std::wstring& text, const RectF& r, float sizeDip, int style,
                           const Color& color, StringAlignment h, StringAlignment v, bool wrap, const wchar_t* family) {
    FontFamily ff(family ? family : textFontFamily());
    Font font(&ff, px(sizeDip), style, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(v);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush brush(color);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, r, &fmt, &brush);
}

void PopupWindow::drawGlyph(Graphics& g, const wchar_t* glyphText, const RectF& r, float sizeDip, const Color& color) {
    drawText(g, glyphText, r, sizeDip, FontStyleRegular, color, StringAlignmentCenter, StringAlignmentCenter, false,
             iconFontFamily());
}

void PopupWindow::drawSpinner(Graphics& g, const RectF& r, float width, const Color& color) {
    float angle = static_cast<float>((::GetTickCount64() % 1000) * 360 / 1000);
    Pen pen(color, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawArc(&pen, r, angle - 90, 90);
}

void PopupWindow::drawProgressRing(Graphics& g, const RectF& r, double progress, bool indeterminate) {
    const float w = px(6);
    RectF ring(r.X + w / 2, r.Y + w / 2, r.Width - w, r.Height - w);
    Pen track(theme_.button, w);
    g.DrawEllipse(&track, ring);
    if (indeterminate) {
        drawSpinner(g, ring, w, theme_.accent);
        return;
    }
    Pen arc(theme_.accent, w);
    arc.SetStartCap(LineCapRound);
    arc.SetEndCap(LineCapRound);
    g.DrawArc(&arc, ring, -90.0f, static_cast<REAL>(360.0 * std::max(0.001, std::min(1.0, progress))));
    drawText(g, std::to_wstring(static_cast<int>(progress * 100)) + L"%", r, 12, FontStyleBold, theme_.text,
             StringAlignmentCenter, StringAlignmentCenter);
}

void PopupWindow::drawPeerGrid(Graphics& g) {
    const auto& peers = app_.peers();
    const RectF& grid = layout_.grid;
    if (peers.empty()) {
        RectF line1(grid.X, grid.Y + grid.Height / 2 - px(22), grid.Width, px(20));
        RectF line2(grid.X, grid.Y + grid.Height / 2 + px(4), grid.Width, px(18));
        if (app_.isSearching()) {
            float d = px(16);
            drawSpinner(g, RectF(grid.X + grid.Width / 2 - d / 2, line1.Y + px(2), d, d), px(2), theme_.secondaryText);
            drawText(g, L"Đang tìm thiết bị…", line2, 12, FontStyleRegular, theme_.secondaryText, StringAlignmentCenter);
        } else {
            drawGlyph(g, glyph::kApp, line1, 20, theme_.secondaryText);
            drawText(g, L"Chưa thấy máy nào cùng mạng Wi-Fi", line2, 12, FontStyleRegular, theme_.secondaryText,
                     StringAlignmentCenter);
        }
        return;
    }

    GraphicsState state = g.Save();
    g.SetClip(grid);
    for (size_t i = 0; i < peers.size(); ++i) {
        RectF r = layout_.tiles[i];
        r.Y += grid.Y - scrollOffset_;
        if (r.Y + r.Height < grid.Y || r.Y > grid.Y + grid.Height) continue;

        const bool selected = peers[i].id == app_.selectedPeerId();
        const bool hovered = hover_ == Hit::Tile && hoverTile_ == static_cast<int>(i);
        GraphicsPath path;
        addRoundRect(path, r, px(10));
        SolidBrush fill(selected ? theme_.accentSoft : (hovered ? theme_.tileHover : theme_.tile));
        g.FillPath(&fill, &path);
        if (selected) {
            Pen border(theme_.accent, px(1.5f));
            g.DrawPath(&border, &path);
        }
        RectF icon(r.X, r.Y + px(10), r.Width, px(36));
        drawGlyph(g, glyphForOs(peers[i].os), icon, 28, selected ? theme_.accent : theme_.text);
        RectF name(r.X + px(4), r.Y + px(48), r.Width - px(8), px(30));
        drawText(g, peers[i].name, name, 11, selected ? FontStyleBold : FontStyleRegular, theme_.text,
                 StringAlignmentCenter, StringAlignmentNear, true);
    }
    g.Restore(state);

    // Thanh cuộn mảnh khi có nhiều hơn 2 hàng.
    float maxScroll = layout_.gridContentHeight - grid.Height;
    if (maxScroll > 0) {
        float barH = grid.Height * grid.Height / layout_.gridContentHeight;
        float barY = grid.Y + (grid.Height - barH) * (scrollOffset_ / maxScroll);
        GraphicsPath bar;
        addRoundRect(bar, RectF(grid.X + grid.Width + px(5), barY, px(3), barH), px(1.5f));
        SolidBrush b(theme_.dashed);
        g.FillPath(&b, &bar);
    }
}

void PopupWindow::drawDropZone(Graphics& g) {
    const RectF& r = layout_.drop;
    GraphicsPath path;
    RectF inner(r.X + px(0.75f), r.Y + px(0.75f), r.Width - px(1.5f), r.Height - px(1.5f));
    addRoundRect(path, inner, px(12));
    SolidBrush fill(dropHover_ ? Color(31, 10, 132, 255) : theme_.tile);
    g.FillPath(&fill, &path);
    Pen dashed(dropHover_ ? theme_.accent : theme_.dashed, px(1.5f));
    REAL dashes[] = {4.0f, 2.7f};
    dashed.SetDashPattern(dashes, 2);
    g.DrawPath(&dashed, &path);

    layout_.cancel = RectF();
    if (const UiTransfer* t = app_.currentTransfer()) {
        float ring = px(56);
        RectF ringRect(r.X + px(16), r.Y + (r.Height - ring) / 2, ring, ring);
        drawProgressRing(g, ringRect, t->progress, t->phase != TransferPhase::Transferring);
        float tx = ringRect.X + ring + px(14);
        float tw = r.X + r.Width - px(16) - tx;
        drawText(g, t->title(), RectF(tx, r.Y + px(30), tw, px(32)), 12, FontStyleBold, theme_.text,
                 StringAlignmentNear, StringAlignmentFar, true);
        drawText(g, t->items, RectF(tx, r.Y + px(64), tw, px(16)), 11, FontStyleRegular, theme_.secondaryText);
        layout_.cancel = RectF(tx, r.Y + px(82), px(40), px(18));
        drawText(g, L"Hủy", layout_.cancel, 11, hover_ == Hit::Cancel ? FontStyleUnderline : FontStyleRegular,
                 theme_.accent);
        return;
    }

    const bool canDrop = app_.canDrop();
    drawGlyph(g, glyph::kDrop, RectF(r.X, r.Y + px(28), r.Width, px(32)), 26,
              canDrop ? theme_.accent : theme_.secondaryText);
    drawText(g, L"Thả file vào đây để gửi", RectF(r.X, r.Y + px(66), r.Width, px(18)), 13, FontStyleBold, theme_.text,
             StringAlignmentCenter);
    std::wstring subtitle;
    if (const UiPeer* p = app_.selectedPeer()) subtitle = L"tới " + p->name;
    else if (auto name = app_.rememberedPeerName(); !name.empty()) subtitle = name + L" hiện không trực tuyến";
    else subtitle = L"Chọn một máy ở trên trước";
    drawText(g, subtitle, RectF(r.X + px(12), r.Y + px(86), r.Width - px(24), px(16)), 11, FontStyleRegular,
             theme_.secondaryText, StringAlignmentCenter);
}

void PopupWindow::drawCheckbox(Graphics& g, const RectF& a, bool checked, bool hovered, const wchar_t* label) {
    RectF box(a.X, a.Y + (a.Height - px(16)) / 2, px(16), px(16));
    GraphicsPath boxPath;
    addRoundRect(boxPath, box, px(4));
    if (checked) {
        SolidBrush fill(theme_.accent);
        g.FillPath(&fill, &boxPath);
        drawGlyph(g, glyph::kCheck, box, 11, Color(255, 255, 255, 255));
    } else {
        SolidBrush fill(hovered ? theme_.tileHover : theme_.background);
        g.FillPath(&fill, &boxPath);
        Pen border(theme_.dashed, px(1));
        g.DrawPath(&border, &boxPath);
    }
    drawText(g, label, RectF(box.X + px(22), a.Y, a.Width - px(22), a.Height), 12, FontStyleRegular, theme_.text);
}

void PopupWindow::drawWebPanel(Graphics& g) {
    const RectF& panel = layout_.webPanel;
    GraphicsPath bg;
    addRoundRect(bg, panel, px(10));
    SolidBrush fill(theme_.accentSoft);
    g.FillPath(&fill, &bg);

    const RectF& u = layout_.webUrl;
    drawGlyph(g, glyph::kGlobe, RectF(u.X, u.Y, px(18), u.Height), 13, theme_.accent);
    drawText(g, app_.webServerUrl(), RectF(u.X + px(22), u.Y, u.Width - px(22), u.Height), 12, FontStyleBold,
             theme_.text, StringAlignmentNear, StringAlignmentCenter, false, L"Consolas");

    auto iconButton = [&](const RectF& r, const wchar_t* gl, bool hovered) {
        GraphicsPath path;
        addRoundRect(path, r, px(6));
        SolidBrush b(hovered ? theme_.tileHover : theme_.button);
        g.FillPath(&b, &path);
        drawGlyph(g, gl, r, 11, theme_.text);
    };
    iconButton(layout_.webCopy, glyph::kCopy, hover_ == Hit::WebCopy);
    iconButton(layout_.webOpen, glyph::kOpenLink, hover_ == Hit::WebOpen);

    if (layout_.webHint.Width > 0) {
        drawText(g, L"Mở địa chỉ này trên trình duyệt của máy khác cùng mạng Wi-Fi. Bạn sẽ được hỏi trước khi máy đó kết nối.",
                 layout_.webHint, 11, FontStyleRegular, theme_.secondaryText, StringAlignmentNear, StringAlignmentNear,
                 true);
    }
    const auto& clients = app_.webClients();
    for (size_t i = 0; i < clients.size() && i < layout_.webRows.size(); ++i) {
        const RectF& r = layout_.webRows[i];
        SolidBrush dot(theme_.success);
        g.FillEllipse(&dot, RectF(r.X, r.Y + (r.Height - px(7)) / 2, px(7), px(7)));
        const std::wstring line = clients[i].name + L"  ·  " + clients[i].ip;
        drawText(g, line, RectF(r.X + px(13), r.Y, r.Width - px(13) - px(40), r.Height), 12, FontStyleRegular,
                 theme_.text);
        const bool hovered = hover_ == Hit::WebDisconnect && hoverTile_ == static_cast<int>(i);
        drawText(g, L"Ngắt", layout_.webDisconnect[i], 11, hovered ? FontStyleUnderline : FontStyleRegular,
                 theme_.accent, StringAlignmentFar);
    }
}

void PopupWindow::drawFooter(Graphics& g) {
    drawCheckbox(g, layout_.webToggle, !app_.webServerUrl().empty(), hover_ == Hit::WebToggle,
                 L"Máy chủ nhận file qua trình duyệt");
    if (layout_.webPanel.Width > 0) drawWebPanel(g);
    drawCheckbox(g, layout_.hiddenToggle, app_.hidden(), hover_ == Hit::Hidden, L"Tạm ẩn — không nhận file");
    drawCheckbox(g, layout_.autostart, app_.autostartEnabled(), hover_ == Hit::Autostart, L"Khởi động cùng Windows");

    // Nút "Thoát"
    GraphicsPath quitPath;
    addRoundRect(quitPath, layout_.quit, px(6));
    SolidBrush quitFill(hover_ == Hit::Quit ? theme_.tileHover : theme_.button);
    g.FillPath(&quitFill, &quitPath);
    drawText(g, L"Thoát", layout_.quit, 12, FontStyleRegular, theme_.text, StringAlignmentCenter);
}

void PopupWindow::paint(HDC hdc) {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    Bitmap buffer(w, h, PixelFormat32bppPARGB);
    Graphics g(&buffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(theme_.background);

    // Header
    const RectF& hd = layout_.header;
    drawGlyph(g, glyph::kApp, RectF(hd.X, hd.Y, px(24), hd.Height), 17, theme_.accent);
    drawText(g, L"FileDrop", RectF(hd.X + px(30), hd.Y, hd.Width - px(70), px(18)), 15, FontStyleBold, theme_.text);
    if (app_.hidden()) {
        drawText(g, L"Đang tạm ẩn · không nhận file", RectF(hd.X + px(30), hd.Y + px(18), hd.Width - px(70), px(14)),
                 11, FontStyleBold, theme_.warning);
    } else {
        drawText(g, app_.deviceName(), RectF(hd.X + px(30), hd.Y + px(18), hd.Width - px(70), px(14)), 11,
                 FontStyleRegular, theme_.secondaryText);
    }
    GraphicsPath refreshPath;
    addRoundRect(refreshPath, layout_.refresh, px(7));
    SolidBrush refreshFill(hover_ == Hit::Refresh ? theme_.tileHover : theme_.button);
    g.FillPath(&refreshFill, &refreshPath);
    drawGlyph(g, glyph::kRefresh, layout_.refresh, 13, theme_.text);

    drawText(g, L"Thiết bị trong mạng", layout_.section, 11, FontStyleBold, theme_.secondaryText);
    drawPeerGrid(g);
    drawDropZone(g);

    if (const UiNote* note = app_.note()) {
        const RectF& s = layout_.status;
        drawGlyph(g, note->isError ? glyph::kError : glyph::kSuccess, RectF(s.X, s.Y, px(16), s.Height), 12,
                  note->isError ? theme_.warning : theme_.success);
        drawText(g, note->text, RectF(s.X + px(20), s.Y, s.Width - px(20), s.Height), 11, FontStyleRegular,
                 theme_.text);
    }

    SolidBrush divider(theme_.divider);
    g.FillRectangle(&divider, layout_.divider);
    drawFooter(g);

    Graphics screen(hdc);
    screen.DrawImage(&buffer, 0, 0, w, h);
}

// --------------------------------------------------------------------------- Chuột / kéo thả

PopupWindow::Hit PopupWindow::hitTest(POINT pt, int* tileIndex) const {
    const PointF p(static_cast<REAL>(pt.x), static_cast<REAL>(pt.y));
    if (tileIndex) *tileIndex = -1;
    if (layout_.refresh.Contains(p)) return Hit::Refresh;
    if (layout_.webToggle.Contains(p)) return Hit::WebToggle;
    if (layout_.webPanel.Width > 0) {
        if (layout_.webCopy.Contains(p)) return Hit::WebCopy;
        if (layout_.webOpen.Contains(p)) return Hit::WebOpen;
        for (size_t i = 0; i < layout_.webDisconnect.size(); ++i) {
            if (layout_.webDisconnect[i].Contains(p)) {
                if (tileIndex) *tileIndex = static_cast<int>(i);
                return Hit::WebDisconnect;
            }
        }
    }
    if (layout_.hiddenToggle.Contains(p)) return Hit::Hidden;
    if (layout_.autostart.Contains(p)) return Hit::Autostart;
    if (layout_.quit.Contains(p)) return Hit::Quit;
    if (layout_.cancel.Width > 0 && layout_.cancel.Contains(p)) return Hit::Cancel;
    if (layout_.grid.Contains(p)) {
        for (size_t i = 0; i < layout_.tiles.size(); ++i) {
            RectF r = layout_.tiles[i];
            r.Y += layout_.grid.Y - scrollOffset_;
            if (r.Contains(p)) {
                if (tileIndex) *tileIndex = static_cast<int>(i);
                return Hit::Tile;
            }
        }
    }
    return Hit::None;
}

bool PopupWindow::pointInDropZone(POINTL screenPt) const {
    POINT pt{screenPt.x, screenPt.y};
    ::ScreenToClient(hwnd_, &pt);
    return layout_.drop.Contains(PointF(static_cast<REAL>(pt.x), static_cast<REAL>(pt.y))) != FALSE;
}

DWORD PopupWindow::dragEffect(POINTL screenPt) {
    return app_.canDrop() && pointInDropZone(screenPt) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

void PopupWindow::setDropHover(bool hover) {
    if (hover != dropHover_) {
        dropHover_ = hover;
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void PopupWindow::dropFiles(const std::vector<std::wstring>& paths, POINTL) {
    if (!paths.empty()) app_.sendFiles(paths);
}

LRESULT CALLBACK PopupWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<PopupWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<PopupWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wParam, lParam) : ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT PopupWindow::handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = ::BeginPaint(hwnd_, &ps);
            paint(hdc);
            ::EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wParam == kAnimTimer) {
                pollOutsideClick();
                if (visible() && animating()) ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_ACTIVATE:
            // Mất focus mà không do chuột (Alt+Tab, phím Windows...) => đóng.
            if (LOWORD(wParam) == WA_INACTIVE && !(::GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
                !(::GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
                hide();
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                ::TrackMouseEvent(&tme);
                trackingMouse_ = true;
            }
            int tile = -1;
            Hit hit = hitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, &tile);
            if (hit != hover_ || tile != hoverTile_) {
                hover_ = hit;
                hoverTile_ = tile;
                ::SetCursor(::LoadCursorW(nullptr, hit == Hit::None ? IDC_ARROW : IDC_HAND));
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                ::SetCursor(::LoadCursorW(nullptr, hover_ == Hit::None ? IDC_ARROW : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            hover_ = Hit::None;
            hoverTile_ = -1;
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            int tile = -1;
            switch (hitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, &tile)) {
                case Hit::Refresh: app_.refresh(); break;
                case Hit::Hidden: app_.toggleHidden(); break;
                case Hit::WebToggle: app_.toggleWebServer(); break;
                case Hit::WebCopy: app_.copyWebServerUrl(); break;
                case Hit::WebOpen: app_.openWebServerUrl(); break;
                case Hit::WebDisconnect:
                    if (tile >= 0 && tile < static_cast<int>(app_.webClients().size()))
                        app_.disconnectWebClient(app_.webClients()[tile].id);
                    break;
                case Hit::Autostart: app_.toggleAutostart(); break;
                case Hit::Quit: app_.quit(); break;
                case Hit::Cancel:
                    if (const UiTransfer* t = app_.currentTransfer()) app_.cancel(t->id);
                    break;
                case Hit::Tile:
                    if (tile >= 0 && tile < static_cast<int>(app_.peers().size())) app_.select(app_.peers()[tile].id);
                    break;
                case Hit::None: break;
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            float maxScroll = layout_.gridContentHeight - layout_.grid.Height;
            if (maxScroll > 0) {
                scrollOffset_ -= GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * px(40);
                scrollOffset_ = std::min(std::max(scrollOffset_, 0.0f), maxScroll);
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) hide();
            return 0;
        case WM_DPICHANGED:
            scale_ = HIWORD(wParam) / 96.0f;
            computeLayout();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            hide();
            return 0;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace filedrop::win
