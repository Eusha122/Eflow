#include "App.h"

#include <shlobj.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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
constexpr int kSoluHotkeyId = 1;                  // Ctrl+Alt+Space: start / end a conversation with Solu
constexpr int kPrivacyHotkeyId = 2;               // Ctrl+Alt+H: hide everything, Solu stops listening (again: back)
constexpr int kAgentsHotkeyId = 4;                // Ctrl+Alt+A: write to Claude Code / Codex from the island
constexpr int kCaptureHotkeyId = 3;               // Ctrl+Alt+Q: drag over part of the screen and Solu explains it
constexpr UINT_PTR kActivityTimer = 2;            // 1 s while the focus timer or a short-lived pill is up
constexpr UINT_PTR kMoodTimer = 3;                // every 30 s: is it late, is the PC struggling?
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
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) return false;   // click-through overlays (Epic Pen, game overlays) are not apps
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
    OleInitialize(nullptr);                  // drag and drop (files onto the island, files out of the shelf)
    if (!gfx_.Init() || !CreateMessageWindow()) return 1;

#ifdef EFLOW_PACKAGED
    ApplyFirstRunDefaults();                 // only installed builds register themselves to start with Windows
#endif
    AddTrayIcon();
    LoadMascot();
    system_ = std::make_unique<SystemMonitor>(msgWnd_, WM_APP_SYSTEM);   // idle until a System tab is on screen
    system_->Start();
    audio_ = std::make_unique<AudioDevices>(msgWnd_, WM_APP_AUDIO);      // samples levels only while the island is open
    audio_->Start();
    camera_ = std::make_unique<CameraService>(msgWnd_, WM_APP_CAMERA);
    camera_->Start();
    solu_ = std::make_unique<SoluService>(msgWnd_, WM_APP_SOLU);
    solu_->Start();                         // starts Solu again if it was switched on last time
    hotkey_ = RegisterHotKey(msgWnd_, kSoluHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE) != FALSE;
    if (!hotkey_) Log("Ctrl+Alt+Space is taken by another program; Solu can still be started from the island");
    if (!RegisterHotKey(msgWnd_, kPrivacyHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H')) Log("Ctrl+Alt+H is taken by another program");
    if (!RegisterHotKey(msgWnd_, kCaptureHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q')) Log("Ctrl+Alt+Q is taken by another program");
    if (!RegisterHotKey(msgWnd_, kAgentsHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A')) Log("Ctrl+Alt+A is taken by another program");
    shelf_ = std::make_unique<ShelfStore>(msgWnd_, WM_APP_SHELF);
    shelf_->Load();
    agents_ = std::make_unique<AgentMonitor>(msgWnd_, WM_APP_AGENTS);
    agents_->Start();
    activityMonitor_ = std::make_unique<ActivityMonitor>(msgWnd_, WM_APP_ACTIVITY);
    activityMonitor_->Start();
    SetTimer(msgWnd_, kMoodTimer, 30000, nullptr);
    BuildIslands();
    media_ = std::make_unique<MediaService>(msgWnd_);
    media_->Start();
    lyrics_ = std::make_unique<LyricsService>(msgWnd_);
    {                                                    // live lyrics are on unless the user switched them off
        std::ifstream in(LyricsSettingPath());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        lyricsOn_ = text.find("false") == std::string::npos;
    }
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
    if (hotkey_) UnregisterHotKey(msgWnd_, kSoluHotkeyId);
    UnregisterHotKey(msgWnd_, kPrivacyHotkeyId);
    UnregisterHotKey(msgWnd_, kCaptureHotkeyId);
    UnregisterHotKey(msgWnd_, kAgentsHotkeyId);
    activityMonitor_.reset();
    agents_.reset();
    shelf_.reset();
    solu_.reset();                           // stops the Solu helper
    audio_.reset();
    camera_.reset();
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
    OleUninitialize();
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

std::wstring App::LyricsSettingPath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) path = std::wstring(base) + L"\\Eflow\\lyrics.json";
    CoTaskMemFree(base);
    return path;
}

void App::OnLyricsChanged() {
    if (!lyrics_) return;
    const Lyrics lyrics = lyrics_->Snapshot();
    for (auto& island : islands_) island->SetLyrics(lyrics);
}

void App::SetLyricsOn(bool on) {
    lyricsOn_ = on;
    {
        std::ofstream out(LyricsSettingPath(), std::ios::trunc);
        out << (on ? "{\"on\": true}\n" : "{\"on\": false}\n");
    }
    for (auto& island : islands_) island->SetLyricsOn(on);
    if (on) OnMediaChanged();                            // look the playing song up now
}

void App::OnMediaChanged() {
    if (!media_) return;
    const MediaInfo info = media_->Snapshot();
#ifndef NDEBUG   // track titles are personal: only debug builds log them
    Log("media: session=%d playing=%d app=%ls title=\"%ls\" artist=\"%ls\" pos=%.0f/%.0fs art=%ux%u controls=%d%d%d seek=%d shuffle=%d/%d repeat=%d/%d",
        info.hasSession, info.playing, info.sourceApp.c_str(), info.title.c_str(), info.artist.c_str(),
        info.PositionNow(), info.duration, info.artwork ? info.artwork->width : 0, info.artwork ? info.artwork->height : 0,
        info.canPrevious, info.canToggle, info.canNext, info.canSeek, info.canShuffle, info.shuffleOn, info.canRepeat, info.repeat);
#endif
    if (lyrics_ && lyricsOn_ && info.hasSession) lyrics_->Request(info.title, info.artist, info.album, info.duration, info.sourceApp);
    for (auto& island : islands_) {
        island->SetMedia(info);
        island->SetMusicPlaying(info.playing);
    }
}

void App::OnPermissionsChanged() {
    if (shoot_) return;                                              // filming: the director decides
    std::vector<PermissionRequest> pending;
    if (permissions_) pending = permissions_->Pending();
    if (solu_)                                                       // Solu's requests share the same card
        for (const PermissionRequest& r : solu_->Approvals()) pending.push_back(r);
    for (auto& island : islands_) island->SetPermissions(pending);
}

void App::OnSystemChanged() {
    if (!system_ || shoot_) return;
    const SystemSnapshot snapshot = system_->Snapshot();
    for (auto& island : islands_) island->SetSystem(snapshot);
}

void App::OnSoluChanged() {
    if (!solu_ || shoot_) return;
    const SoluSnapshot snapshot = solu_->Snapshot();
    for (auto& island : islands_) island->SetSolu(snapshot);
    if (showAnswer_ && (snapshot.state == SoluState::Thinking || snapshot.state == SoluState::Speaking)) {
        showAnswer_ = false;                                         // the answer to a quick capture: show it
        for (auto& island : islands_) island->ShowForSolu();
    }
    for (const SoluAppEvent& e : solu_->TakeAppEvents()) OnSoluAppEvent(e);
    if (snapshot.approvalsVersion != soluApprovalsVersion_) {
        soluApprovalsVersion_ = snapshot.approvalsVersion;
        OnPermissionsChanged();
    }
    if (snapshot.sleepRequested) solu_->Enable(false);               // "Solu, go to sleep"
}

void App::OnSoluHotkey() {
    if (!solu_ || shoot_) return;
    // Open the island where the pointer is, so the user sees the conversation start.
    POINT pt;
    GetCursorPos(&pt);
    const HMONITOR here = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    for (auto& island : islands_)
        if (island->monitor() == here || islands_.size() == 1) island->ShowForSolu();
    solu_->Talk();
}

// ---- live activities -------------------------------------------------------------------------

// ---- agents (Claude Code and Codex) ------------------------------------------------------------

void App::OnShelfChanged() {
    if (!shelf_ || shoot_) return;
    const ShelfSnapshot snap = shelf_->Snapshot();
    for (auto& island : islands_) island->SetShelf(snap);
}

void App::OnAgentsChanged() {
    if (!agents_ || shoot_) return;
    AgentSnapshot snap = agents_->Snapshot();
    const uint64_t now = GetTickCount64();
    if (now - hookCheck_ > 5000) {                                                  // are the approval cards connected (a file read: not on every change)
        hookCheck_ = now;
        claudeHooked_ = HookInstaller::IsConnected(AiTool::ClaudeCode);
        codexHooked_ = HookInstaller::IsConnected(AiTool::Codex);
    }
    snap.claudeHooked = claudeHooked_;
    snap.codexHooked = codexHooked_;
    // A session that waits for an approval is "waiting for you".
    if (permissions_)
        for (const PermissionRequest& r : permissions_->Pending())
            for (AgentSession& s : snap.sessions)
                if ((r.source == L"Claude Code") == (s.tool == AgentTool::Claude) && (r.cwd.empty() || r.cwd == s.cwd || r.project == s.project))
                    if (s.status == AgentStatus::Working) s.status = AgentStatus::Waiting, s.doing = L"Waiting for you";
    // A session that just finished: the cat cheers and a pill says so (unless the Agents tab is what you are looking at).
    for (const AgentSession& s : snap.sessions) {
        const auto it = agentWas_.find(s.tool == AgentTool::Claude ? L"c:" + s.id : L"x:" + s.id);
        const bool was = it != agentWas_.end() && it->second == AgentStatus::Working;
        agentWas_[s.tool == AgentTool::Claude ? L"c:" + s.id : L"x:" + s.id] = s.status;
        if (was && s.status == AgentStatus::Idle) {
            Activity a;
            a.id = L"agentdone:" + s.id + std::to_wstring(now);
            a.kind = Activity::Kind::Done;
            a.glyph = L"";
            a.title = s.tool == AgentTool::Claude ? L"Claude finished" : L"Codex finished";
            a.detail = s.project;
            a.accent = s.tool == AgentTool::Claude ? 0xD97757 : 0x30D158;
            a.dismissible = true;
            a.expiresAt = now + 8000;
            doneItems_.push_back(std::move(a));
            CheerAll();
        }
    }
    agentSnap_ = snap;
    for (auto& island : islands_) island->SetAgents(snap);
    if (solu_) {                                                                     // Solu can say what they are doing
        auto esc = [](const std::wstring& w) {
            std::string out;
            for (wchar_t c : w) {
                if (c == L'"' || c == L'\\') out += '\\', out += static_cast<char>(c);
                else if (c < 0x20) out += ' ';
                else if (c < 0x80) out += static_cast<char>(c);
                else {
                    char buf[8];
                    const int n8 = WideCharToMultiByte(CP_UTF8, 0, &c, 1, buf, 8, nullptr, nullptr);
                    out.append(buf, n8 > 0 ? n8 : 0);
                }
            }
            return out;
        };
        std::string json = "{\"agents\":[";
        int count = 0;
        for (const AgentSession& s : snap.sessions) {
            if (count >= 6) break;
            const char* state = s.status == AgentStatus::Working ? "working" : s.status == AgentStatus::Waiting ? "waiting for the user" : "idle";
            json += std::string(count++ ? "," : "") + "{\"agent\":\"" + (s.tool == AgentTool::Claude ? "Claude Code" : "Codex") + "\",\"project\":\"" +
                    esc(s.project) + "\",\"title\":\"" + esc(s.title) + "\",\"state\":\"" + state + "\",\"doing\":\"" + esc(s.doing) +
                    "\",\"last_reply\":\"" + esc(s.lastReply.substr(0, 300)) + "\"}";
        }
        json += "]}";
        if (json != lastAgentContext_) {
            lastAgentContext_ = json;
            solu_->SendContext(json);
        }
    }
    PushActivities();
}

void App::OnActivityChanged() {
    if (!activityMonitor_) return;
    const uint64_t now = GetTickCount64();
    for (const std::wstring& name : activityMonitor_->TakeFinished()) {          // a download or copy just finished
        Activity a;
        a.id = L"done:" + name + std::to_wstring(now);
        a.kind = Activity::Kind::Done;
        a.glyph = L"";                                                        // check mark
        a.title = name == L"Copy" ? L"Copied" : L"Downloaded";
        a.detail = name == L"Copy" ? L"All files are in place" : name;
        a.accent = 0x30D158;
        a.dismissible = true;
        a.expiresAt = now + 5000;
        doneItems_.push_back(std::move(a));
        CheerAll();
    }
    PushActivities();
}

void App::PushActivities() {
    if (shoot_) return;
    const uint64_t now = GetTickCount64();
    auto alive = [&](const Activity& a) { return (!a.expiresAt || a.expiresAt > now) && !dismissed_.count(a.id); };
    std::erase_if(doneItems_, [&](const Activity& a) { return !alive(a); });
    std::erase_if(soluActivities_, [&](const Activity& a) { return !alive(a); });
    std::vector<Activity> all;
    for (const Activity& a : doneItems_) all.push_back(a);                         // most important first
    for (const Activity& a : soluActivities_) if (a.kind == Activity::Kind::Nudge) all.push_back(a);
    if (focus_.on) {
        Activity f;
        f.id = L"focus";
        f.kind = Activity::Kind::Timer;
        f.glyph = focus_.onBreak ? L"" : L"";                         // a cup of tea / a stopwatch
        f.title = focus_.onBreak ? L"Break" : L"Focus";
        f.detail = L"Round " + std::to_wstring(focus_.round) + L" of " + std::to_wstring(focus_.rounds);
        f.accent = focus_.onBreak ? 0x64D2FF : 0xBF5AF2;
        f.endsAt = focus_.endsAt;
        f.length = focus_.length;
        all.push_back(std::move(f));
    }
    for (const Activity& a : soluActivities_) if (a.kind != Activity::Kind::Nudge) all.push_back(a);
    for (const AgentSession& s : agentSnap_.sessions) {                                // an agent is working: its pill, with what it is doing
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const ULONGLONG unixNow = (((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000;
        if (s.status != AgentStatus::Working || unixNow - std::min(unixNow, s.lastActivity) > 5 * 60 * 1000) continue;
        Activity a;
        a.id = L"agent:" + s.id;
        a.kind = Activity::Kind::Progress;
        a.glyph = s.tool == AgentTool::Claude ? L"" : L"";
        a.title = s.tool == AgentTool::Claude ? L"Claude Code" : L"Codex";
        a.detail = s.doing;
        a.accent = s.tool == AgentTool::Claude ? 0xD97757 : 0x64D2FF;
        all.push_back(std::move(a));
    }
    if (activityMonitor_)
        for (const Activity& a : activityMonitor_->Snapshot()) all.push_back(a);
    for (auto& island : islands_) island->SetActivities(all);
    const bool ticking = focus_.on || !doneItems_.empty() || std::any_of(soluActivities_.begin(), soluActivities_.end(),
                                                                         [](const Activity& a) { return a.expiresAt != 0; });
    if (ticking) SetTimer(msgWnd_, kActivityTimer, 1000, nullptr);
    else KillTimer(msgWnd_, kActivityTimer);
}

void App::OnSoluAppEvent(const SoluAppEvent& e) {
    const uint64_t now = GetTickCount64();
    if (e.name == "activity") {                                                     // research and other long jobs
        Activity a;
        a.id = e.Get("id");
        a.title = e.Get("title");
        a.detail = e.Get("detail");
        const std::wstring p = e.Get("progress");
        a.progress = p.empty() ? -1.0f : static_cast<float>(_wtof(p.c_str()));
        a.glyph = e.Get("id") == L"research" ? L"" : L"";              // magnifier / lightning
        a.accent = 0xBF5AF2;
        std::erase_if(soluActivities_, [&](const Activity& x) { return x.id == a.id; });
        soluActivities_.push_back(std::move(a));
    } else if (e.name == "activity_end") {
        const std::wstring id = e.Get("id");
        std::erase_if(soluActivities_, [&](const Activity& x) { return x.id == id; });
        if (e.Get("done") == L"true") {
            Activity d;
            d.id = L"done:" + id + std::to_wstring(now);
            d.kind = Activity::Kind::Done;
            d.glyph = L"";
            d.title = e.Get("title").empty() ? L"Done" : e.Get("title");
            d.detail = e.Get("detail");
            d.accent = 0x30D158;
            d.dismissible = true;
            d.expiresAt = now + 6000;
            doneItems_.push_back(std::move(d));
            CheerAll();
        }
    } else if (e.name == "nudge") {                                                 // rare and quiet: a pill, never a voice
        Activity n;
        n.id = L"nudge:" + std::to_wstring(++nudgeSeq_);
        n.kind = Activity::Kind::Nudge;
        n.glyph = L"";                                                        // light bulb
        n.title = e.Get("title").empty() ? L"Solu" : e.Get("title");
        n.detail = e.Get("text");
        n.accent = 0xFFB340;
        n.dismissible = true;
        n.expiresAt = now + 60000;
        soluActivities_.push_back(std::move(n));
    } else if (e.name == "focus") {
        const int minutes = _wtoi(e.Get("minutes").c_str());
        if (minutes <= 0) StopFocus(false);
        else StartFocus(minutes, std::max(1, _wtoi(e.Get("break").c_str())), std::max(1, _wtoi(e.Get("rounds").c_str())));
    } else if (e.name == "capture") {
        StartCapture(e.Get("mode").empty() ? L"explain" : e.Get("mode"));
    } else if (e.name == "cheer") {
        CheerAll();
    } else if (e.name == "agent_prompt" && agents_) {                              // "Solu, tell Claude to ..."
        const AgentTool tool = e.Get("tool") == L"codex" ? AgentTool::Codex : AgentTool::Claude;
        const std::wstring project = e.Get("project");
        const AgentSession* pick = nullptr;
        for (const AgentSession& s : agentSnap_.sessions) {                          // the project they named, else the agent's latest
            if (s.tool != tool) continue;
            if (!project.empty() && _wcsicmp(s.project.c_str(), project.c_str()) == 0) { pick = &s; break; }
            if (!pick) pick = &s;
        }
        std::wstring error;
        if (!agents_->Send(tool, pick ? pick->id : L"", pick ? pick->cwd : L"", e.Get("text"), false, error))
            Log("agent: Solu could not send the prompt: %ls", error.c_str());
    }
    PushActivities();
}

void App::StartFocus(int focusMinutes, int breakMinutes, int rounds) {
    focus_ = {};
    focus_.on = true;
    focus_.focusMin = focusMinutes;
    focus_.breakMin = breakMinutes;
    focus_.rounds = rounds;
    focus_.length = static_cast<uint64_t>(focusMinutes) * 60000;
    focus_.endsAt = GetTickCount64() + focus_.length;
    Log("focus: %d min focus, %d min break, %d rounds", focusMinutes, breakMinutes, rounds);
    UpdateMood();
    PushActivities();
}

void App::StopFocus(bool finished) {
    if (!focus_.on) return;
    focus_.on = false;
    if (finished && solu_) solu_->Say(L"That's all your focus rounds. Great work today!");
    UpdateMood();
    PushActivities();
}

void App::TickFocus() {
    if (!focus_.on || GetTickCount64() < focus_.endsAt) return;
    if (!focus_.onBreak) {                                                          // a focus round is over: a break
        CheerAll();
        if (focus_.round >= focus_.rounds) {
            StopFocus(true);
            return;
        }
        focus_.onBreak = true;
        focus_.length = static_cast<uint64_t>(focus_.breakMin) * 60000;
        if (solu_) solu_->Say(L"Nice focus! Take a " + std::to_wstring(focus_.breakMin) + L" minute break.");
    } else {                                                                        // the break is over
        focus_.onBreak = false;
        ++focus_.round;
        focus_.length = static_cast<uint64_t>(focus_.focusMin) * 60000;
        if (solu_) solu_->Say(L"Break's over. Back to focus!");
    }
    focus_.endsAt = GetTickCount64() + focus_.length;
    UpdateMood();
}

void App::CheerAll() {
    for (auto& island : islands_) island->Cheer();
}

void App::UpdateMood() {
    if (shoot_) return;                                                             // filming: the director sets the mood
    // Sleepy late at night and during focus breaks; worried when the PC has been flat out for a minute.
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto u64 = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const uint64_t i = u64(idle), total = u64(kernel) + u64(user);
        if (cpuTotal_ && total > cpuTotal_) {
            const double busy = 1.0 - static_cast<double>(i - cpuIdle_) / static_cast<double>(total - cpuTotal_);
            busyChecks_ = busy > 0.90 ? busyChecks_ + 1 : 0;
        }
        cpuIdle_ = i;
        cpuTotal_ = total;
    }
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    Mascot::Mood mood = Mascot::Mood::Normal;
    if (busyChecks_ >= 2 || mem.dwMemoryLoad >= 94) mood = Mascot::Mood::Worried;
    else if ((focus_.on && focus_.onBreak) || local.tm_hour >= 23 || local.tm_hour < 5) mood = Mascot::Mood::Sleepy;
    for (auto& island : islands_) island->SetMood(mood);
}

void App::TogglePrivacy() {
    privacy_ = !privacy_;
    Log("privacy mode %s", privacy_ ? "on" : "off");
    for (auto& island : islands_) ShowWindow(island->hwnd(), privacy_ ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (solu_) {
        if (privacy_) solu_->StopSpeaking();
        solu_->SetPaused(privacy_);
    }
    if (!privacy_) UpdateFullscreenState();                                         // a fullscreen app may still want it hidden
}

void App::StartCapture(const std::wstring& mode) {
    if (privacy_ || capture_.Active()) return;
    capture_.Start(mode, [this](RECT r, const std::wstring& m) {
        captured_ = r;
        capturedMode_ = m;
        PostMessageW(msgWnd_, WM_APP_CAPTURED, 0, 0);
    });
}

void App::OnCameraChanged() {
    if (!camera_ || shoot_) return;
    const CameraSnapshot snapshot = camera_->Snapshot();
    for (auto& island : islands_) island->SetCamera(snapshot);
}

void App::OnAudioChanged() {
    if (!audio_ || shoot_) return;
    const AudioSnapshot snapshot = audio_->Snapshot();
    for (auto& island : islands_) island->SetAudio(snapshot);
}

void App::OnNotificationsChanged() {
    if (!notifications_ || shoot_) return;
    const NotificationSnapshot snap = notifications_->Snapshot();
#ifndef NDEBUG
    Log("notifications: access=%d count=%zu arrivals=%llu", static_cast<int>(snap.access), snap.items.size(),
        static_cast<unsigned long long>(snap.arrivals));
#endif
    const bool arrived = snap.arrivals > seenArrivals_;
    seenArrivals_ = snap.arrivals;
    if (solu_) {                                                     // what Solu may mention: who wrote, in which app (not the text)
        std::string json = "{\"notifications\":[";
        for (size_t i = 0; i < snap.items.size() && i < 30; ++i) {
            const auto& n = snap.items[i];
            auto esc = [](const std::wstring& w) {
                std::string out;
                for (wchar_t c : w) {
                    if (c == L'"' || c == L'\\') out += '\\', out += static_cast<char>(c);
                    else if (c < 0x20) out += ' ';
                    else if (c < 0x80) out += static_cast<char>(c);
                    else {                                            // UTF-8
                        char buf[8];
                        const int n8 = WideCharToMultiByte(CP_UTF8, 0, &c, 1, buf, 8, nullptr, nullptr);
                        out.append(buf, n8 > 0 ? n8 : 0);
                    }
                }
                return out;
            };
            json += std::string(i ? "," : "") + "{\"app\":\"" + esc(n.app) + "\",\"title\":\"" + esc(n.title) + "\"}";
        }
        solu_->SendContext(json + "]}");
    }
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
        island->SetAudioHandlers(
            [this](bool watching) {
                if (audio_) audio_->Watch(watching ? +1 : -1);
                if (camera_) camera_->Watch(watching ? +1 : -1);
            },
            [this](bool input, const std::wstring& id) {
                if (shoot_) return ShootSelect(input, id);
                if (audio_) audio_->Select(input, id);
            },
            [this](int side, bool muted) {                      // 0 microphone, 1 speaker, 2 camera (privacy switch), 3 Solu (off = muted)
                if (shoot_) return ShootMute(side, muted);      // filming: nothing is really muted
                if (side == 3) {
                    if (solu_) solu_->Enable(!muted);
                } else if (side == 2) {
                    if (camera_) camera_->SetBlocked(muted);
                } else if (audio_) {
                    audio_->SetMute(side == 0, muted);
                }
            },
            [this](bool input, bool start) {                    // the "Test" button in the microphone / speaker menu
                if (!audio_ || shoot_) return;
                if (start) audio_->StartTest(input);
                else audio_->StopTest();
            });
        island->SetSoluHandlers(
            [this](bool on) { if (solu_ && !shoot_) solu_->Enable(on); },
            [this] { OnSoluHotkey(); },
            [this](const std::wstring& voice) { if (solu_ && !shoot_) solu_->SetVoice(voice); },
            [this](bool headphones) { if (solu_) solu_->SetHeadphones(headphones); },
            [this] { if (solu_) solu_->StopSpeaking(); },
            [this](bool captions) { if (solu_) solu_->SetCaptions(captions); },
            [this](bool screen) { if (solu_) solu_->SetScreen(screen); });
        island->SetMediaHandler([this](MediaCommand cmd, double seconds) {
            if (media_) media_->Send(cmd, seconds);
        });
        island->SetNotificationHandlers(
            [this](uint32_t id) {
                if (shoot_) return ShootDismissNotification(id);
                if (notifications_) notifications_->Dismiss(id);
            },
            [this] {
                if (shoot_) {
                    shootNotifs_.items.clear();
                    for (auto& island : islands_) island->SetNotifications(shootNotifs_);
                    return;
                }
                if (notifications_) notifications_->ClearAll();
            });
        island->SetAgentHandlers(
            [this](AgentTool tool, const std::wstring& session, const std::wstring& cwd, const std::wstring& text, bool fresh, std::wstring& error) {
                if (shoot_) return true;                        // filming: the director plays the agent's answer
                return agents_ && agents_->Send(tool, session, cwd, text, fresh, error);
            },
            [this](AgentTool tool) { ToggleAiTool(tool == AgentTool::Claude ? AiTool::ClaudeCode : AiTool::Codex); });
        island->SetLyricsOn(lyricsOn_);
        island->SetLyricsHandler([this](bool on) { SetLyricsOn(on); });
        island->SetTaskHandler([this](const std::string& op, const std::string& id) { if (solu_ && !shoot_) solu_->TaskControl(op, id); });
        island->SetShelfHandlers([this](const std::vector<std::wstring>& paths) {
                                     if (shoot_) {                       // filming: the real shelf is left alone
                                         for (const auto& p : paths) ShootShelfAdd(p);
                                         for (auto& island : islands_) island->SetShelf(shootShelf_);
                                     } else if (shelf_) shelf_->Add(paths);
                                 },
                                 [this](const std::wstring& path) {
                                     if (shoot_) {
                                         std::erase_if(shootShelf_.items, [&](const ShelfItem& i) { return i.path == path; });
                                         ++shootShelf_.version;
                                         for (auto& island : islands_) island->SetShelf(shootShelf_);
                                     } else if (shelf_) shelf_->Remove(path);
                                 },
                                 [this] { if (shelf_ && !shoot_) shelf_->Clear(); });
        island->SetAgentOpen([this](const AgentSession& s, bool openChat) {
            AgentSession copy = s;
            std::thread([copy, openChat] { AgentMonitor::FocusHost(copy, openChat); }).detach();         // (it sleeps briefly: not on the UI thread)
        });
        island->SetActivityDismiss([this](const std::wstring& id) {
            if (shoot_) {
                std::erase_if(shootActs_, [&](const Activity& a) { return a.id == id; });
                for (auto& island : islands_) island->SetActivities(shootActs_);
                return;
            }
            dismissed_.insert(id);
            PushActivities();
        });
        island->SetPermissionHandler([this](uint64_t id, PermissionDecision decision) {
            if (shoot_) return ShootDecide(id);                 // filming: nothing is really approved
            if (id >= kSoluApprovalBase) {
                if (solu_) solu_->Answer(id, decision);
                return;
            }
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
    OnShelfChanged();
    OnMediaChanged();                        // fresh islands start with the current track
    OnLyricsChanged();                       // ... and its lyrics (the lookup is not repeated for the same song)
    OnAgentsChanged();                       // the agents tab and the live pills (it pushes the activities too)
    OnAudioChanged();
    OnCameraChanged();
    OnSoluChanged();
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
        } else if (wp == kActivityTimer) {
            TickFocus();
            PushActivities();
        } else if (wp == kMoodTimer) {
            UpdateMood();
        }
        return 0;
    case WM_APP_ACTIVITY:
        OnActivityChanged();
        return 0;
    case WM_APP_AGENTS:
        OnAgentsChanged();
        return 0;
    case WM_APP_SHELF:
        OnShelfChanged();
        return 0;
    case WM_APP_CAPTURED:
        if (solu_ && !shoot_) {
            showAnswer_ = true;
            solu_->LookAtRegion(captured_, capturedMode_);
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
    case WM_APP_LYRICS:
        OnLyricsChanged();
        return 0;
    case WM_APP_NOTIFICATION:
        OnNotificationsChanged();
        return 0;
    case WM_APP_SYSTEM:
        OnSystemChanged();
        return 0;
    case WM_APP_AUDIO:
        OnAudioChanged();
        return 0;
    case WM_APP_CAMERA:
        OnCameraChanged();
        return 0;
    case WM_APP_SOLU:
        OnSoluChanged();
        return 0;
    case WM_HOTKEY:
        if (wp == kSoluHotkeyId) OnSoluHotkey();
        else if (wp == kPrivacyHotkeyId) TogglePrivacy();
        else if (wp == kCaptureHotkeyId) StartCapture(L"explain");
        else if (wp == kAgentsHotkeyId) {                                          // open where the pointer is, cursor in the prompt box
            POINT pt;
            GetCursorPos(&pt);
            const HMONITOR here = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
            for (auto& island : islands_)
                if (island->monitor() == here || islands_.size() == 1) island->FocusAgentPrompt();
        }
        return 0;
#ifndef NDEBUG
    case WM_APP_DEBUG_SOLU:                  // scripted tests: 1 on, 0 off, 2 talk, 3 ask, 4 stop
        if (solu_) {
            if (wp == 1) solu_->Enable(true);
            else if (wp == 0) solu_->Enable(false);
            else if (wp == 2) OnSoluHotkey();
            else if (wp == 3) solu_->Ask(L"Hey Solu, tell me something cheerful in two short sentences.");
            else if (wp == 4) solu_->StopSpeaking();
            else if (wp == 5) solu_->Ask(L"Turn off the live captions");
            else if (wp == 6) solu_->Ask(L"Turn on the live captions");
            else if (wp == 7) solu_->Ask(L"Use the tiny voice");
            else if (wp == 8) solu_->Ask(L"Solu, what does this error mean?");
            else if (wp == 9) solu_->Ask(L"Open the calculator");
            else if (wp == 10) solu_->Ask(L"Close the calculator window");
            else if (wp == 11) solu_->Ask(L"Research the benefits of drinking water, then email a short summary to test@example.com");
            else if (wp == 12) {                                        // a test nudge
                SoluAppEvent e;
                e.name = "nudge";
                e.fields["title"] = L"Tomorrow";
                e.fields["text"] = L"Your exam is on October 20th";
                OnSoluAppEvent(e);
            } else if (wp == 13) StartCapture(L"explain");
            else if (wp == 14) solu_->Ask(L"Start a focus session for 1 minute with 1 minute breaks and 2 rounds");
            else if (wp == 15) solu_->Ask(L"Start a task: research three facts about honey bees, then write them into a file called bee-test.txt in my Documents folder");
        }
        return 0;
#endif
    case WM_APP_PERMISSION:
        OnPermissionsChanged();
        return 0;
#ifndef NDEBUG
    case WM_COPYDATA: {                      // shoot mode: the director's commands (UTF-8 JSON)
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cds && cds->dwData == 0x45464C57 && cds->lpData)
            OnShootCommand(std::string(static_cast<const char*>(cds->lpData), cds->cbData));
        return TRUE;
    }
#endif
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
