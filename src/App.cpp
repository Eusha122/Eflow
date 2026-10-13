#include "App.h"

#include <string>
#include <utility>

#include "core/Messages.h"

namespace eflow {
namespace {

App* g_app = nullptr;                      // WinEvent callbacks carry no user pointer

const wchar_t kMsgClass[] = L"Eflow.App";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kAppKey[] = L"Software\\Eflow";
const wchar_t kRunValue[] = L"Eflow";

constexpr UINT_PTR kDisplayDebounceTimer = 1;   // display changes arrive in bursts
constexpr UINT kMenuAutostart = 1;
constexpr UINT kMenuQuit = 2;

std::wstring ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) { path.resize(n); return path; }
        path.resize(path.size() * 2);
    }
}

bool IsShellWindow(HWND hwnd) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0;
}

// True when `hwnd` covers the whole of `monitor` (games, video players, F11 browsers, presentations).
bool CoversMonitor(HWND hwnd, HMONITOR monitor) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsShellWindow(hwnd)) return false;
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != monitor) return false;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

} // namespace

int App::Run() {
    // One island per desktop session.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Eflow.SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_app = this;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (!gfx_.Init() || !CreateMessageWindow()) return 1;

#ifdef NDEBUG
    ApplyFirstRunDefaults();                 // dev builds never register themselves to start with Windows
#endif
    AddTrayIcon();
    BuildIslands();

    // Fullscreen guard, event-driven: the shell's own fullscreen notification (sent to app bars, the
    // same signal the taskbar uses) plus foreground/minimize changes. No polling, no per-move events.
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = msgWnd_;
    abd.uCallbackMessage = WM_APP_APPBAR;
    appBar_ = SHAppBarMessage(ABM_NEW, &abd) != FALSE;   // registered without a position: reserves no space
    for (auto [first, last] : {std::pair{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                               std::pair{EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND}}) {
        if (HWINEVENTHOOK h = SetWinEventHook(first, last, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT))
            hooks_.push_back(h);
    }
    UpdateFullscreenState();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (HWINEVENTHOOK h : hooks_) UnhookWinEvent(h);
    if (appBar_) {
        APPBARDATA remove{sizeof(remove)};
        remove.hWnd = msgWnd_;
        SHAppBarMessage(ABM_REMOVE, &remove);
    }
    islands_.clear();
    Shell_NotifyIconW(NIM_DELETE, &tray_);
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}

bool App::CreateMessageWindow() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMsgClass;
    if (!RegisterClassExW(&wc)) return false;
    // A hidden top-level window (not message-only) so it receives broadcasts: display changes, TaskbarCreated.
    msgWnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMsgClass, L"Eflow", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                              wc.hInstance, this);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    return msgWnd_ != nullptr;
}

void App::BuildIslands() {
    islands_.clear();
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(m);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    for (HMONITOR m : monitors) {
        auto island = std::make_unique<IslandWindow>(gfx_, m, msgWnd_);
        if (island->Create()) islands_.push_back(std::move(island));
    }
}

void App::RebuildGraphics() {
    islands_.clear();
    if (!gfx_.Init()) {                      // GPU still gone: retry shortly
        SetTimer(msgWnd_, kDisplayDebounceTimer, 1000, nullptr);
        return;
    }
    BuildIslands();
    UpdateFullscreenState();
}

void App::UpdateFullscreenState() {
    HWND fg = GetForegroundWindow();
    for (auto& island : islands_) {
        const bool fullscreen = CoversMonitor(fg, island->monitor());
        island->SetSuppressed(fullscreen);
        if (!fullscreen)                     // other topmost windows can bury us; re-assert
            SetWindowPos(island->hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void CALLBACK App::OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG idObject, LONG, DWORD, DWORD) {
    if (g_app && idObject == OBJID_WINDOW) g_app->UpdateFullscreenState();
}

// ---- tray ------------------------------------------------------------------------------------

void App::AddTrayIcon() {
    tray_ = NOTIFYICONDATAW{sizeof(tray_)};
    tray_.hWnd = msgWnd_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    tray_.uCallbackMessage = WM_APP_TRAY;
    tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);   // placeholder until the app icon exists
    wcscpy_s(tray_.szTip, L"Eflow");
    Shell_NotifyIconW(NIM_ADD, &tray_);
    tray_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &tray_);
}

void App::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit Eflow");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(msgWnd_);            // required so the menu closes when clicking elsewhere
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, msgWnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == kMenuAutostart) SetAutostart(!AutostartEnabled());
    else if (cmd == kMenuQuit) PostQuitMessage(0);
}

// ---- autostart -------------------------------------------------------------------------------

bool App::AutostartEnabled() {
    wchar_t value[MAX_PATH * 2]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return false;
    return std::wstring(value) == L"\"" + ExePath() + L"\"";
}

void App::SetAutostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        const std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

void App::ApplyFirstRunDefaults() {
    // Eflow is meant to always be there: start with Windows unless the user has turned it off before.
    HKEY key;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key,
                        &disposition) != ERROR_SUCCESS)
        return;
    DWORD initialized = 0, size = sizeof(initialized);
    if (RegQueryValueExW(key, L"Initialized", nullptr, nullptr, reinterpret_cast<BYTE*>(&initialized), &size) !=
            ERROR_SUCCESS || !initialized) {
        SetAutostart(true);
        initialized = 1;
        RegSetValueExW(key, L"Initialized", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&initialized), sizeof(initialized));
    }
    RegCloseKey(key);
}

// ---- window procedure ------------------------------------------------------------------------

LRESULT CALLBACK App::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->WndProc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreated_ && taskbarCreated_) {   // Explorer restarted: the tray icon is gone
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        SetTimer(hwnd, kDisplayDebounceTimer, 300, nullptr);
        return 0;
    case WM_TIMER:
        if (wp == kDisplayDebounceTimer) {
            KillTimer(hwnd, kDisplayDebounceTimer);
            RebuildGraphics();
        }
        return 0;
    case WM_APP_DEVICE_LOST:
        SetTimer(hwnd, kDisplayDebounceTimer, 100, nullptr);
        return 0;
    case WM_APP_APPBAR:
        if (wp == ABN_FULLSCREENAPP) UpdateFullscreenState();
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
        return 0;
    case WM_ENDSESSION:
        if (wp) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace eflow
