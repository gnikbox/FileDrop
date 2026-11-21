// FileDrop (Windows) — App: icon khay hệ thống, cầu nối Engine, hỏi nhận file, thông báo.
#pragma once

#include <windows.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "filedrop/engine.h"

namespace filedrop::win {

class PopupWindow;

inline constexpr UINT WM_APP_RUN = WM_APP + 1;   // chạy std::function* từ luồng nền
inline constexpr UINT WM_APP_TRAY = WM_APP + 2;  // sự kiện icon khay
inline constexpr UINT WM_APP_SHOW = WM_APP + 3;  // instance thứ 2 yêu cầu hiện popup
inline constexpr UINT kTrayIconId = 1;
inline constexpr wchar_t kMessageWindowClass[] = L"FileDropMessageWindow";

struct UiPeer {
    std::string id;
    std::wstring name;
    std::string os;
};

struct UiTransfer {
    std::string id;
    Direction direction = Direction::Send;
    std::wstring peerName;
    std::wstring items;  // “ảnh.jpg” hoặc "3 file"
    TransferPhase phase = TransferPhase::WaitingForAccept;
    double progress = 0;
    ULONGLONG startedAt = 0;

    std::wstring title() const;
};

struct UiNote {
    std::wstring text;
    bool isError = false;
};

class App {
public:
    App();
    ~App();

    int run(HINSTANCE instance, bool autostart);

    // --- Trạng thái cho PopupWindow ---
    const std::vector<UiPeer>& peers() const { return peers_; }
    const std::string& selectedPeerId() const { return selectedPeerId_; }
    const UiPeer* selectedPeer() const;
    std::wstring rememberedPeerName() const;
    const UiTransfer* currentTransfer() const;
    const UiNote* note() const { return note_ ? &*note_ : nullptr; }
    bool isSearching() const { return searching_; }
    const std::wstring& deviceName() const { return deviceName_; }
    bool autostartEnabled() const { return autostart_; }
    bool canDrop() const { return selectedPeer() != nullptr; }
    HWND messageWindow() const { return msgWnd_; }

    // --- Hành động từ UI ---
    void refresh();
    void select(const std::string& peerId);
    void sendFiles(const std::vector<std::wstring>& paths);
    void cancel(const std::string& transferId);
    void toggleAutostart();
    void quit();

private:
    static LRESULT CALLBACK messageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void post(std::function<void()> fn);
    void startEngine();

    // Sự kiện từ Engine (đã chuyển về UI thread).
    void onPeerUpdated(const Peer& p);
    void onPeerLost(const std::string& id);
    void onIncomingRequest(const TransferInfo& info);
    void onPhase(const TransferInfo& info, TransferPhase phase);
    void onProgress(const TransferInfo& info, uint64_t done, uint64_t total);
    void onFinished(const TransferInfo& info, const TransferResult& result);

    void presentNextIncoming();
    void showNote(const std::wstring& text, bool isError, bool sticky = false);
    void notify(const std::wstring& title, const std::wstring& body, const std::wstring& revealPath = {});
    void addTrayIcon();
    void updateTrayIcon();
    void showTrayMenu();
    void changed();

    HINSTANCE instance_ = nullptr;
    HWND msgWnd_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    HICON trayIcon_ = nullptr;
    double trayProgress_ = -2;
    std::unique_ptr<PopupWindow> popup_;
    std::unique_ptr<Engine> engine_;

    std::wstring deviceName_;
    std::vector<UiPeer> peers_;
    std::string selectedPeerId_;
    std::map<std::string, UiTransfer> transfers_;
    std::optional<UiNote> note_;
    std::deque<TransferInfo> incomingQueue_;
    bool presentingIncoming_ = false;
    bool searching_ = false;
    bool autostart_ = false;
    std::wstring revealPath_;
};

}  // namespace filedrop::win
