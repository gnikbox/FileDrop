#include "App.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Drawing.h"
#include "PopupWindow.h"
#include "Settings.h"
#include "resource.h"
#include "filedrop/util.h"

namespace filedrop::win {

namespace {

constexpr UINT_PTR kNoteTimer = 1;
constexpr UINT_PTR kSearchTimer = 2;
constexpr UINT kMenuOpen = 100;
constexpr UINT kMenuQuit = 101;
constexpr int kAcceptButton = 100;
constexpr int kDeclineButton = 101;

std::wstring itemsDescription(const TransferInfo& info) {
    if (info.fileCount == 1 && !info.fileName.empty()) return L"“" + fromUtf8(info.fileName) + L"”";
    return std::to_wstring(info.fileCount) + L" file";
}

std::wstring formatBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"byte", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1000 && u < 4) {
        v /= 1000;
        ++u;
    }
    wchar_t buf[64];
    if (u == 0) swprintf(buf, 64, L"%llu byte", static_cast<unsigned long long>(bytes));
    else swprintf(buf, 64, L"%.1f %s", v, units[u]);
    return buf;
}

HRESULT CALLBACK taskDialogCallback(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR) {
    if (msg == TDN_CREATED) {
        ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        ::SetForegroundWindow(hwnd);
    }
    return S_OK;
}

}  // namespace

std::wstring UiTransfer::title() const {
    if (direction == Direction::Send) {
        switch (phase) {
            case TransferPhase::WaitingForAccept: return L"Đang chờ " + peerName + L" chấp nhận…";
            case TransferPhase::Connecting: return L"Đang kết nối tới " + peerName + L"…";
            case TransferPhase::Transferring: return L"Đang gửi tới " + peerName;
        }
    }
    return phase == TransferPhase::Transferring ? L"Đang nhận từ " + peerName : L"Đang chờ " + peerName + L" kết nối…";
}

App::App() = default;

App::~App() {
    if (engine_) engine_->stop();
    if (trayIcon_) ::DestroyIcon(trayIcon_);
}

int App::run(HINSTANCE instance, bool /*autostart*/) {
    instance_ = instance;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &App::messageProc;
    wc.hInstance = instance;
    wc.lpszClassName = kMessageWindowClass;
    wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    ::RegisterClassExW(&wc);
    // Cửa sổ ẩn (không dùng HWND_MESSAGE để nhận được broadcast "TaskbarCreated").
    auto fatal = [](const wchar_t* what) {
        std::wstring text = std::wstring(what) + L"\n\nMã lỗi Windows: " + std::to_wstring(::GetLastError());
        ::MessageBoxW(nullptr, text.c_str(), L"FileDrop không khởi động được", MB_OK | MB_ICONERROR);
        return 1;
    };

    if (::CreateWindowExW(0, kMessageWindowClass, L"FileDrop", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                          this) == nullptr) {
        return fatal(L"Không tạo được cửa sổ nền.");
    }
    taskbarCreatedMsg_ = ::RegisterWindowMessageW(L"TaskbarCreated");

    autostart_ = isAutostartEnabled();
    selectedPeerId_ = toUtf8(readSetting(L"SelectedPeerId"));

    popup_ = std::make_unique<PopupWindow>(*this);
    if (!popup_->create(instance)) return fatal(L"Không tạo được cửa sổ popup.");

    addTrayIcon();
    startEngine();

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (engine_) engine_->stop();
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = msgWnd_;
    nid.uID = kTrayIconId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    return 0;
}

void App::startEngine() {
    std::wstring deviceId = readSetting(L"DeviceId");
    if (deviceId.empty()) {
        deviceId = fromUtf8(randomHex(8));
        writeSetting(L"DeviceId", deviceId);
    }
    EngineConfig cfg;
    cfg.deviceId = toUtf8(deviceId);
    cfg.deviceName = defaultDeviceName();
    cfg.os = "windows";
    cfg.downloadDir = toUtf8(downloadsFolder());
    deviceName_ = fromUtf8(cfg.deviceName);

    EngineCallbacks cb;
    cb.onPeerUpdated = [this](const Peer& p) { post([this, p] { onPeerUpdated(p); }); };
    cb.onPeerLost = [this](const std::string& id) { post([this, id] { onPeerLost(id); }); };
    cb.onIncomingRequest = [this](const TransferInfo& i) { post([this, i] { onIncomingRequest(i); }); };
    cb.onTransferPhase = [this](const TransferInfo& i, TransferPhase ph) { post([this, i, ph] { onPhase(i, ph); }); };
    cb.onProgress = [this](const TransferInfo& i, uint64_t d, uint64_t t) { post([this, i, d, t] { onProgress(i, d, t); }); };
    cb.onTransferFinished = [this](const TransferInfo& i, const TransferResult& r) {
        post([this, i, r] { onFinished(i, r); });
    };

    engine_ = std::make_unique<Engine>(cfg, cb);
    std::string err;
    if (!engine_->start(&err)) {
        showNote(L"Không khởi động được mạng: " + fromUtf8(err), true, true);
        return;
    }
    refresh();
}

void App::post(std::function<void()> fn) {
    auto* heap = new std::function<void()>(std::move(fn));
    if (!msgWnd_ || !::PostMessageW(msgWnd_, WM_APP_RUN, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
}

LRESULT CALLBACK App::messageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* app = static_cast<App*>(cs->lpCreateParams);
        // Gán HWND ngay tại đây: handleMessage() gọi DefWindowProcW(msgWnd_, ...) cho các message
        // gửi tới trong lúc CreateWindowExW chưa trả về (WM_NCCREATE phải trả TRUE).
        app->msgWnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* self = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->handleMessage(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {  // Explorer khởi động lại
        addTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_APP_RUN: {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lParam));
            if (fn && *fn) (*fn)();
            return 0;
        }
        case WM_APP_SHOW:
            popup_->show();
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (!popup_->visible()) refresh();
                    popup_->toggle();
                    break;
                case WM_CONTEXTMENU:
                    showTrayMenu();
                    break;
                case NIN_BALLOONUSERCLICK:
                    if (!revealPath_.empty()) {
                        std::wstring args = L"/select,\"" + revealPath_ + L"\"";
                        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                    break;
            }
            return 0;
        case WM_TIMER:
            if (wParam == kNoteTimer) {
                ::KillTimer(msgWnd_, kNoteTimer);
                note_.reset();
                changed();
            } else if (wParam == kSearchTimer) {
                ::KillTimer(msgWnd_, kSearchTimer);
                searching_ = false;
                changed();
            }
            return 0;
        case WM_SETTINGCHANGE:
            trayProgress_ = -2;  // đổi theme => vẽ lại icon
            updateTrayIcon();
            popup_->themeChanged();
            return 0;
    }
    return ::DefWindowProcW(msgWnd_, msg, wParam, lParam);
}

// --------------------------------------------------------------------------- Trạng thái

const UiPeer* App::selectedPeer() const {
    for (const auto& p : peers_) {
        if (p.id == selectedPeerId_) return &p;
    }
    return nullptr;
}

std::wstring App::rememberedPeerName() const {
    if (const UiPeer* p = selectedPeer()) return p->name;
    return readSetting(L"SelectedPeerName");
}

const UiTransfer* App::currentTransfer() const {
    const UiTransfer* best = nullptr;
    for (const auto& [id, t] : transfers_) {
        if (!best || t.startedAt >= best->startedAt) best = &t;
    }
    return best;
}

void App::changed() {
    if (popup_) popup_->invalidate();
}

// --------------------------------------------------------------------------- Hành động

void App::refresh() {
    if (engine_) engine_->discover();
    searching_ = true;
    ::SetTimer(msgWnd_, kSearchTimer, 3000, nullptr);
    changed();
}

void App::select(const std::string& peerId) {
    selectedPeerId_ = peerId;
    writeSetting(L"SelectedPeerId", fromUtf8(peerId));
    if (const UiPeer* p = selectedPeer()) writeSetting(L"SelectedPeerName", p->name);
    changed();
}

void App::sendFiles(const std::vector<std::wstring>& paths) {
    const UiPeer* peer = selectedPeer();
    if (!peer) {
        showNote(L"Hãy chọn một máy để gửi trước", true);
        return;
    }
    std::vector<std::string> utf8;
    for (const auto& p : paths) utf8.push_back(toUtf8(p));
    std::string err;
    if (engine_->sendFiles(peer->id, utf8, &err).empty()) showNote(fromUtf8(err), true);
}

void App::cancel(const std::string& transferId) {
    if (engine_) engine_->cancel(transferId);
}

void App::toggleAutostart() {
    if (setAutostart(!autostart_)) {
        autostart_ = isAutostartEnabled();
    } else {
        showNote(L"Không đổi được chế độ khởi động cùng Windows", true);
    }
    changed();
}

void App::quit() {
    popup_->hide();
    ::PostQuitMessage(0);
}

// --------------------------------------------------------------------------- Sự kiện Engine

void App::onPeerUpdated(const Peer& p) {
    UiPeer ui{p.id, fromUtf8(p.name), p.os};
    auto it = std::find_if(peers_.begin(), peers_.end(), [&](const UiPeer& x) { return x.id == p.id; });
    if (it != peers_.end()) {
        *it = ui;
    } else {
        peers_.push_back(ui);
        std::sort(peers_.begin(), peers_.end(), [](const UiPeer& a, const UiPeer& b) {
            return ::CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                                     a.name.c_str(), -1, b.name.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
        });
    }
    if (p.id == selectedPeerId_) writeSetting(L"SelectedPeerName", ui.name);
    changed();
}

void App::onPeerLost(const std::string& id) {
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const UiPeer& p) { return p.id == id; }),
                 peers_.end());
    changed();
}

void App::onIncomingRequest(const TransferInfo& info) {
    incomingQueue_.push_back(info);
    presentNextIncoming();
}

void App::onPhase(const TransferInfo& info, TransferPhase phase) {
    auto it = transfers_.find(info.id);
    if (it == transfers_.end()) {
        UiTransfer t;
        t.id = info.id;
        t.direction = info.direction;
        t.peerName = fromUtf8(info.peer.name);
        t.items = itemsDescription(info);
        t.startedAt = ::GetTickCount64();
        it = transfers_.emplace(info.id, t).first;
    }
    it->second.phase = phase;
    updateTrayIcon();
    changed();
}

void App::onProgress(const TransferInfo& info, uint64_t done, uint64_t total) {
    auto it = transfers_.find(info.id);
    if (it == transfers_.end()) return;
    it->second.phase = TransferPhase::Transferring;
    it->second.progress = total == 0 ? 1.0 : static_cast<double>(done) / static_cast<double>(total);
    updateTrayIcon();
    changed();
}

void App::onFinished(const TransferInfo& info, const TransferResult& r) {
    transfers_.erase(info.id);
    incomingQueue_.erase(std::remove_if(incomingQueue_.begin(), incomingQueue_.end(),
                                        [&](const TransferInfo& x) { return x.id == info.id; }),
                         incomingQueue_.end());
    updateTrayIcon();

    const std::wstring peer = fromUtf8(info.peer.name);
    const std::wstring items = itemsDescription(info);
    const std::wstring message = fromUtf8(r.message);
    switch (r.status) {
        case TransferStatus::Completed:
            if (info.direction == Direction::Send) {
                showNote(L"Đã gửi xong " + items + L" tới " + peer, false);
                notify(L"Đã gửi xong", items + L" đã được gửi tới " + peer + L".");
            } else {
                showNote(L"Đã nhận " + items + L" từ " + peer, false);
                std::wstring reveal = fromUtf8(r.savedPaths.empty() ? r.saveDir : r.savedPaths.front());
                notify(L"Đã nhận xong", items + L" từ " + peer + L" đã lưu vào Downloads.", reveal);
            }
            break;
        case TransferStatus::Declined:
            if (info.direction == Direction::Send) showNote(peer + L" đã từ chối nhận file", true);
            break;
        case TransferStatus::Cancelled:
            showNote(message.empty() ? L"Đã hủy" : message, true);
            break;
        case TransferStatus::TimedOut:
        case TransferStatus::Failed:
            showNote(message.empty() ? L"Truyền file thất bại" : message, true);
            if (info.direction == Direction::Receive || r.status == TransferStatus::Failed) {
                notify(L"Truyền file thất bại", message);
            }
            break;
    }
    changed();
}

// --------------------------------------------------------------------------- Hỏi nhận file

void App::presentNextIncoming() {
    if (presentingIncoming_ || incomingQueue_.empty()) return;
    TransferInfo info = incomingQueue_.front();
    incomingQueue_.pop_front();
    presentingIncoming_ = true;

    const std::wstring peer = fromUtf8(info.peer.name);
    const std::wstring title = L"Nhận file từ " + peer + L"?";
    const std::wstring content =
        peer + L" muốn gửi cho bạn " + itemsDescription(info) + L" (" + formatBytes(info.totalBytes) + L").";

    TASKDIALOG_BUTTON buttons[] = {{kAcceptButton, L"Chấp nhận"}, {kDeclineButton, L"Từ chối"}};
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hwndParent = nullptr;
    cfg.hInstance = instance_;
    cfg.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = L"FileDrop";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = title.c_str();
    cfg.pszContent = content.c_str();
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.pButtons = buttons;
    cfg.nDefaultButton = kAcceptButton;
    cfg.pfCallback = taskDialogCallback;

    int pressed = kDeclineButton;
    if (FAILED(::TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr))) {
        pressed = ::MessageBoxW(nullptr, (title + L"\n\n" + content).c_str(), L"FileDrop",
                                MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES
                      ? kAcceptButton
                      : kDeclineButton;
    }
    const bool accepted = pressed == kAcceptButton;
    if (!engine_->respond(info.id, accepted) && accepted) showNote(L"Yêu cầu từ " + peer + L" đã hết hạn", true);

    presentingIncoming_ = false;
    post([this] { presentNextIncoming(); });
}

// --------------------------------------------------------------------------- Khay hệ thống

void App::addTrayIcon() {
    trayProgress_ = -2;
    updateTrayIcon();
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = msgWnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = trayIcon_;
    wcscpy_s(nid.szTip, L"FileDrop");
    ::Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void App::updateTrayIcon() {
    const UiTransfer* t = currentTransfer();
    double progress = t ? (t->phase == TransferPhase::Transferring ? t->progress : 0.0) : -1.0;
    // Chỉ vẽ lại khi thay đổi đáng kể (mỗi 2%).
    if (trayProgress_ > -2 && ((progress < 0) == (trayProgress_ < 0)) && std::abs(progress - trayProgress_) < 0.02) return;
    trayProgress_ = progress;

    HICON old = trayIcon_;
    trayIcon_ = nullptr;
    if (progress < 0) {
        // Lúc rảnh: dùng đúng icon của app (đúng kích thước theo DPI).
        ::LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), LIM_SMALL, &trayIcon_);
    }
    if (!trayIcon_) trayIcon_ = createTrayIcon(::GetSystemMetrics(SM_CXSMICON), taskbarUsesDarkTheme(), progress);
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = msgWnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON;
    nid.hIcon = trayIcon_;
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (old) ::DestroyIcon(old);
}

void App::showTrayMenu() {
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, kMenuOpen, L"Mở FileDrop");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuQuit, L"Thoát");
    POINT pt;
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(msgWnd_);
    UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, msgWnd_, nullptr);
    ::DestroyMenu(menu);
    if (cmd == kMenuOpen) popup_->show();
    if (cmd == kMenuQuit) quit();
}

void App::showNote(const std::wstring& text, bool isError, bool sticky) {
    note_ = UiNote{text, isError};
    ::KillTimer(msgWnd_, kNoteTimer);
    if (!sticky) ::SetTimer(msgWnd_, kNoteTimer, 6000, nullptr);
    changed();
}

void App::notify(const std::wstring& title, const std::wstring& body, const std::wstring& revealPath) {
    revealPath_ = revealPath;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = msgWnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

}  // namespace filedrop::win
