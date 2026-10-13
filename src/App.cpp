#include "App.h"

#include <filesystem>
#include <string>
#include <utility>

#include "core/Log.h"
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
constexpr UINT kMenuConnectClaude = 3;
constexpr UINT kMenuConnectCodex = 4;
constexpr UINT kMenuMascotFirst = 10;           // Mascot test submenu: Idle, Music, Notification, AI request
constexpr float kMascotTextureScale = 0.25f;    // 2048-px art -> 512 px: plenty for a ~40-DIP mascot at 300%

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

#ifdef EFLOW_PACKAGED
    ApplyFirstRunDefaults();                 // only installed builds register themselves to start with Windows
#endif
    AddTrayIcon();
    LoadMascot();
    system_ = std::make_unique<SystemMonitor>(msgWnd_, WM_APP_SYSTEM);   // idle until a System tab is on screen
    system_->Start();
    BuildIslands();
    media_ = std::make_unique<MediaService>(msgWnd_);
    media_->Start();
    permissions_ = std::make_unique<PermissionServer>(msgWnd_);
    permissions_->Start();
    notifications_ = std::make_unique<NotificationService>(msgWnd_);
    notifications_->Start();

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

    system_.reset();
    notifications_.reset();
    permissions_.reset();                    // pending requests fall back to each tool's own prompt
    media_.reset();                          // joins the media worker thread
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

void App::LoadMascot() {
    // The asset ships next to the executable. Without it Eflow still runs, just without the mascot.
    const std::filesystem::path file = std::filesystem::path(ExePath()).parent_path() / L"assets" / L"mascot" / L"eflow.glb";
    auto asset = std::make_shared<MascotAsset>();
    std::string error;
    if (!asset->Load(file, kMascotTextureScale, error)) {
        Log("mascot disabled: %s", error.c_str());
        return;
    }
    Log("mascot loaded: %zu meshes, %zu clips", asset->meshes.size(), asset->clips.size());
    mascotAsset_ = std::move(asset);
}

void App::SetMascotState(Mascot::State state) {
    for (auto& island : islands_) island->SetMascotState(state);
}

void App::OnMediaChanged() {
    if (!media_) return;
    const MediaInfo info = media_->Snapshot();
#ifndef NDEBUG   // track titles are personal: only debug builds log them
    Log("media: session=%d playing=%d app=%ls title=\"%ls\" artist=\"%ls\" pos=%.0f/%.0fs art=%ux%u controls=%d%d%d",
        info.hasSession, info.playing, info.sourceApp.c_str(), info.title.c_str(), info.artist.c_str(),
        info.PositionNow(), info.duration, info.artwork ? info.artwork->width : 0, info.artwork ? info.artwork->height : 0,
        info.canPrevious, info.canToggle, info.canNext);
#endif
    for (auto& island : islands_) {
        island->SetMedia(info);
        island->SetMusicPlaying(info.playing);
    }
}

void App::OnPermissionsChanged() {
    if (!permissions_) return;
    const auto pending = permissions_->Pending();
    for (auto& island : islands_) island->SetPermissions(pending);
}

void App::OnSystemChanged() {
    if (!system_) return;
    const SystemSnapshot snapshot = system_->Snapshot();
    for (auto& island : islands_) island->SetSystem(snapshot);
}

void App::OnNotificationsChanged() {
    if (!notifications_) return;
    const NotificationSnapshot snap = notifications_->Snapshot();
#ifndef NDEBUG
    Log("notifications: access=%d count=%zu arrivals=%llu", static_cast<int>(snap.access), snap.items.size(),
        static_cast<unsigned long long>(snap.arrivals));
#endif
    const bool arrived = snap.arrivals > seenArrivals_;
    seenArrivals_ = snap.arrivals;
    for (auto& island : islands_) {
        island->SetNotifications(snap);
        if (arrived) island->OnNotification();
    }
}

void App::ToggleAiTool(AiTool tool) {
    const wchar_t* name = tool == AiTool::ClaudeCode ? L"Claude Code" : L"Codex";
    const bool connected = HookInstaller::IsConnected(tool);
    const std::wstring file = HookInstaller::ConfigPath(tool).wstring();
    std::wstring prompt = connected
        ? std::wstring(L"Stop sending ") + name + L" permission requests to Eflow?\n\nEflow will remove its hook from:\n" + file
        : std::wstring(L"Send ") + name + L" permission requests to Eflow?\n\n" + name +
              L" will ask in the island instead of its own prompt. If Eflow is closed or you don't answer, " + name +
              L" asks as usual. Nothing is ever approved automatically.\n\nEflow will add one hook entry to:\n" + file +
              L"\n(a backup of the original is kept next to it)";
    if (tool == AiTool::Codex && !connected)
        prompt += L"\n\nCodex asks you to trust new hooks: run /hooks in Codex once after connecting.";
    if (MessageBoxW(msgWnd_, prompt.c_str(), L"Eflow", MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND) != IDOK) return;
    std::wstring error;
    const bool ok = connected ? HookInstaller::Disconnect(tool, error) : HookInstaller::Connect(tool, error);
    if (!ok) MessageBoxW(msgWnd_, error.c_str(), L"Eflow", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

void App::BuildIslands() {
    islands_.clear();
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(m);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    // GPU resources for the mascot belong to the current device; (re)create them with it.
    mascotReady_ = mascotAsset_ && mascotRenderer_.Init(gfx_.d3d(), *mascotAsset_);
    if (mascotAsset_ && !mascotReady_) Log("mascot renderer failed to initialise; continuing without it");
    Log("building islands for %zu monitor(s)", monitors.size());
    for (HMONITOR m : monitors) {
        auto island = std::make_unique<IslandWindow>(gfx_, m, msgWnd_, mascotReady_ ? mascotAsset_ : nullptr,
                                                     mascotReady_ ? &mascotRenderer_ : nullptr);
        if (!island->Create()) continue;
        island->SetSystemWatchHandler([this](bool watching) {
            if (system_) system_->Watch(watching ? +1 : -1);
        });
        island->SetMediaHandler([this](MediaCommand cmd) {
            if (media_) media_->Send(cmd);
        });
        island->SetNotificationHandlers(
            [this](uint32_t id) {
                if (notifications_) notifications_->Dismiss(id);
            },
            [this] {
                if (notifications_) notifications_->ClearAll();
            });
        island->SetPermissionHandler([this](uint64_t id, PermissionDecision decision) {
            if (permissions_) permissions_->Resolve(id, decision);
        });
        islands_.push_back(std::move(island));
    }
}

void App::RebuildGraphics() {
    islands_.clear();
    if (!gfx_.Init()) {                      // GPU still gone: retry shortly
        SetTimer(msgWnd_, kDisplayDebounceTimer, 1000, nullptr);
        return;
    }
    BuildIslands();
    OnMediaChanged();                        // fresh islands start with the current track
    OnPermissionsChanged();
    OnNotificationsChanged();
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
    HMENU ai = CreatePopupMenu();
    AppendMenuW(ai, MF_STRING | (HookInstaller::IsConnected(AiTool::ClaudeCode) ? MF_CHECKED : 0), kMenuConnectClaude,
                L"Claude Code");
    AppendMenuW(ai, MF_STRING | (HookInstaller::IsConnected(AiTool::Codex) ? MF_CHECKED : 0), kMenuConnectCodex, L"Codex");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(ai), L"Approve AI requests from");
    if (mascotReady_) {                      // temporary: trigger reactions until real events are wired up
        HMENU test = CreatePopupMenu();
        const wchar_t* labels[] = {L"Idle", L"Music playing", L"New notification", L"AI permission request"};
        for (UINT i = 0; i < 4; ++i) AppendMenuW(test, MF_STRING, kMenuMascotFirst + i, labels[i]);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(test), L"Mascot test");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit Eflow");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(msgWnd_);            // required so the menu closes when clicking elsewhere
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, msgWnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == kMenuAutostart) SetAutostart(!AutostartEnabled());
    else if (cmd == kMenuQuit) PostQuitMessage(0);
    else if (cmd == kMenuConnectClaude) ToggleAiTool(AiTool::ClaudeCode);
    else if (cmd == kMenuConnectCodex) ToggleAiTool(AiTool::Codex);
    else if (cmd >= kMenuMascotFirst && cmd < kMenuMascotFirst + 4)
        SetMascotState(static_cast<Mascot::State>(cmd - kMenuMascotFirst));
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
        Log("GPU device lost; rebuilding");
        SetTimer(hwnd, kDisplayDebounceTimer, 100, nullptr);
        return 0;
#ifndef NDEBUG
    case WM_APP_DEBUG_MASCOT:
        Log("debug: mascot state %u -> %zu island(s)", static_cast<unsigned>(wp), islands_.size());
        if (wp <= static_cast<WPARAM>(Mascot::State::Permission)) SetMascotState(static_cast<Mascot::State>(wp));
        return 0;
#endif
    case WM_APP_MEDIA:
        OnMediaChanged();
        return 0;
    case WM_APP_NOTIFICATION:
        OnNotificationsChanged();
        return 0;
    case WM_APP_SYSTEM:
        OnSystemChanged();
        return 0;
    case WM_APP_PERMISSION:
        OnPermissionsChanged();
        return 0;
#ifndef NDEBUG
    case WM_APP_DEBUG_TAB:
        for (auto& island : islands_) island->DebugOpenTab(static_cast<int>(static_cast<INT_PTR>(wp)));
        return 0;
#endif
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
