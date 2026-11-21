// FileDrop (Windows) — điểm khởi động.
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <string>

#include "App.h"
#include "Drawing.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    // Chỉ cho phép 1 instance; instance sau sẽ yêu cầu instance đang chạy hiện popup.
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, L"Local\\FileDrop.SingleInstance");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = ::FindWindowW(filedrop::win::kMessageWindowClass, nullptr)) {
            ::PostMessageW(existing, filedrop::win::WM_APP_SHOW, 0, 0);
        }
        return 0;
    }

    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(::OleInitialize(nullptr))) return 1;

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    const bool autostart = cmdLine && std::wstring(cmdLine).find(L"--autostart") != std::wstring::npos;
    int code = 0;
    {
        filedrop::win::App app;
        code = app.run(instance, autostart);
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    ::OleUninitialize();
    if (mutex) ::CloseHandle(mutex);
    return code;
}
