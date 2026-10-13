#include "island/IslandWindow.h"

#include <d2d1effects.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "core/Log.h"
#include "core/Messages.h"
#include "mascot/MascotRenderer.h"

namespace eflow {
namespace {

// ---- design tokens (DIPs) --------------------------------------------------------------------
constexpr float kTopInset = 6.0f;          // gap between the screen edge and the island
constexpr float kMargin = 26.0f;           // room around the island for the shadow and spring overshoot
constexpr float kCollapsedW = 196.0f;
constexpr float kCollapsedH = 7.0f;
constexpr float kExpandedW = 560.0f;
constexpr float kExpandedH = 176.0f;
constexpr float kMaxRadius = 24.0f;
constexpr float kHotSlackX = 14.0f;        // extra hover area around the idle bar
constexpr float kHotSlackY = 9.0f;
constexpr float kLeaveSlack = 6.0f;        // how far outside the panel still counts as "inside"

constexpr float kTabTop = 14.0f;
constexpr float kTabH = 30.0f;
constexpr float kTabPadX = 14.0f;
constexpr float kTabGap = 4.0f;

constexpr UINT kOpenDelayMs = 70;          // hover intent before opening
constexpr UINT kCloseDelayMs = 380;        // grace period before closing
constexpr UINT_PTR kOpenTimer = 1;
constexpr UINT_PTR kCloseTimer = 2;
constexpr UINT_PTR kMascotTimer = 3;       // paces mascot frames while the panel is open and still
constexpr UINT_PTR kAutoCloseTimer = 4;    // closes a panel that opened itself for a notification
constexpr UINT kMascotCalmMs = 33;         // ~30 fps for idle breathing/blinks
constexpr UINT kMascotActiveMs = 16;       // ~60 fps while reacting
constexpr UINT kAutoCloseMs = 3200;

// Mascot placement inside the open panel. Model space: 1 unit = 1000 px of the 1024-px art canvas,
// y up, origin at its centre.
constexpr float kMascotH = 92.0f;          // on-screen height of the full art (ears + antenna to tail)
constexpr float kMascotArtH = 0.672f;      // model-space height of the art
constexpr float kMascotK = kMascotH / kMascotArtH;   // DIPs per model unit
constexpr float kMascotPawsY = -0.268f;    // model y of the paw bottoms
constexpr float kMascotFromRight = 92.0f;  // mascot centre, measured from the panel's right edge
constexpr float kMascotFloor = 12.0f;      // paw bottoms above the panel's bottom edge
constexpr float kMascotRise = 12.0f;       // it rises this far into place as the panel opens
constexpr float kMascotLane = 172.0f;      // panel width reserved on the right for the mascot

constexpr D2D1_COLOR_F Rgba(float r, float g, float b, float a) { return {r, g, b, a}; }
constexpr D2D1_COLOR_F kIslandFill = Rgba(0.051f, 0.051f, 0.063f, 1.0f);
constexpr D2D1_COLOR_F kText = Rgba(0.96f, 0.96f, 0.98f, 1.0f);
constexpr D2D1_COLOR_F kTextDim = Rgba(0.96f, 0.96f, 0.98f, 0.52f);

constexpr int kHitPrev = 10, kHitPlay = 11, kHitNext = 12;   // media control hit ids
constexpr int kHitReject = 20, kHitApprove = 21, kHitDefer = 22;  // permission card hit ids
constexpr int kHitRing = 40;                                 // + metric index (System tab)
constexpr int kHitNotifClear = 30;                          // "Clear all"
constexpr int kHitNotifClose = 100;                         // + notification index: its dismiss button
constexpr float kNotifRowH = 44.0f;
constexpr float kNotifListTop = 54.0f;                      // from the panel top
constexpr float kNotifRows = 2.0f;                          // rows visible at once
constexpr float kArtSize = 92.0f;
constexpr float kArtTop = 62.0f;
constexpr float kControlsY = 145.0f;       // centre line of the transport buttons (from the panel top)

const wchar_t* const kTabLabels[kSectionCount] = {L"Music", L"Notifications", L"AI Requests", L"System"};
const wchar_t* const kEmptyTitle[kSectionCount] = {L"Nothing playing", L"You're all caught up", L"No pending requests", L""};
const wchar_t* const kEmptyBody[kSectionCount] = {
    L"Play something in Spotify, YouTube or your browser",
    L"New notifications will show up here",
    L"Claude Code and Codex approvals will appear here",
    L"",
};

// ---- System tab: what each ring can show -----------------------------------------------------
constexpr D2D1_COLOR_F kAccent = Rgba(0.42f, 0.70f, 1.0f, 1.0f);
constexpr D2D1_COLOR_F kAmber = Rgba(1.0f, 0.73f, 0.30f, 1.0f);
constexpr D2D1_COLOR_F kRed = Rgba(1.0f, 0.42f, 0.38f, 1.0f);
constexpr float kRingRadius = 27.0f;
constexpr float kRingThickness = 6.0f;

struct SystemPage {
    std::wstring value, unit, caption;     // centre text (unit sits under the value), line under the label
    float fraction = 0;                    // ring fill, 0..1
    int severity = 0;                      // 0 calm, 1 warm, 2 hot
};
struct Metric {
    const wchar_t* label;
    std::vector<SystemPage> pages;
};

std::wstring Fmt(const wchar_t* format, double a) {
    wchar_t buffer[48];
    swprintf_s(buffer, format, a);
    return buffer;
}
std::wstring Fmt2(const wchar_t* format, double a, double b) {
    wchar_t buffer[64];
    swprintf_s(buffer, format, a, b);
    return buffer;
}
int PercentSeverity(float p) { return p >= 90 ? 2 : p >= 70 ? 1 : 0; }
int TempSeverity(float t) { return t >= 85 ? 2 : t >= 70 ? 1 : 0; }
constexpr double kGiB = 1073741824.0;

// Bits per second -> "12.4" + "Mb/s".
void Rate(double bps, std::wstring& value, std::wstring& unit) {
    if (bps < 1e3) { value = Fmt(L"%.0f", bps); unit = L"b/s"; }
    else if (bps < 1e6) { value = Fmt(L"%.0f", bps / 1e3); unit = L"Kb/s"; }
    else if (bps < 1e9) { value = Fmt(bps < 1e8 ? L"%.1f" : L"%.0f", bps / 1e6); unit = L"Mb/s"; }
    else { value = Fmt(L"%.2f", bps / 1e9); unit = L"Gb/s"; }
}
std::wstring RateText(double bps) {
    std::wstring v, u;
    Rate(bps, v, u);
    return v + L" " + u;
}
double NiceCeil(double v) {                // 1, 2, 5 x 10^n: keeps the network ring's scale steady
    const double e = std::pow(10.0, std::floor(std::log10(v)));
    const double m = v / e;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * e;
}

std::vector<Metric> BuildMetrics(const SystemSnapshot& s) {
    const bool ok = s.valid;
    const SystemPage dash{L"\u2014", L"", L"", 0, 0};
    std::vector<Metric> m;

    // CPU: usage -> temperature -> fan
    Metric cpu{L"CPU", {}};
    SystemPage u = ok ? SystemPage{Fmt(L"%.0f%%", s.cpuUsage), L"", Has(s.cpuClockGHz) ? Fmt(L"%.1f GHz", s.cpuClockGHz)
                                                                                         : Fmt(L"%.0f threads", s.cpuThreads),
                                   s.cpuUsage / 100.0f, PercentSeverity(s.cpuUsage)}
                        : dash;
    cpu.pages.push_back(u);
    SystemPage t = dash;
    t.caption = L"Needs sensor app";
    if (ok && Has(s.cpuTempC)) t = {Fmt(L"%.0f", s.cpuTempC), L"\u00B0C", L"Temperature", s.cpuTempC / 100.0f, TempSeverity(s.cpuTempC)};
    else if (ok && s.sensorBridge) t.caption = L"Not reported";
    cpu.pages.push_back(t);
    SystemPage f = dash;
    f.caption = L"Needs sensor app";
    if (ok && !s.fans.empty()) {
        const FanReading* best = &s.fans[0];
        for (const auto& fan : s.fans)
            if (fan.rpm > best->rpm) best = &fan;
        f = {Fmt(L"%.0f", best->rpm), L"RPM", s.fans.size() > 1 ? Fmt(L"Fastest of %.0f", static_cast<double>(s.fans.size())) : L"Fan",
             std::min(1.0f, best->rpm / 3000.0f), 0};
    } else if (ok && s.sensorBridge) {
        f.caption = L"No fans found";
    }
    cpu.pages.push_back(f);
    m.push_back(std::move(cpu));

    // GPU: usage -> temperature -> fan -> memory
    Metric gpu{L"GPU", {}};
    if (ok && s.gpuAvailable) {
        gpu.pages.push_back({Fmt(L"%.0f%%", s.gpuUsage), L"", Has(s.gpuTempC) ? Fmt(L"%.0f\u00B0C", s.gpuTempC) : L"Graphics",
                             s.gpuUsage / 100.0f, PercentSeverity(s.gpuUsage)});
        gpu.pages.push_back(Has(s.gpuTempC) ? SystemPage{Fmt(L"%.0f", s.gpuTempC), L"\u00B0C", L"Temperature", s.gpuTempC / 100.0f,
                                                        TempSeverity(s.gpuTempC)}
                                            : SystemPage{L"\u2014", L"", L"Not reported", 0, 0});
        gpu.pages.push_back(Has(s.gpuFanPercent) ? SystemPage{Fmt(L"%.0f%%", s.gpuFanPercent), L"", L"Fan speed", s.gpuFanPercent / 100.0f, 0}
                                                 : SystemPage{L"\u2014", L"", L"Not reported", 0, 0});
        if (s.vramTotal) {
            const float used = static_cast<float>(static_cast<double>(s.vramUsed) / static_cast<double>(s.vramTotal));
            gpu.pages.push_back({Fmt(L"%.1f", s.vramUsed / kGiB), L"GB", Fmt(L"of %.0f GB", s.vramTotal / kGiB), used,
                                 PercentSeverity(used * 100.0f)});
        } else {
            gpu.pages.push_back({L"\u2014", L"", L"Memory", 0, 0});
        }
    } else {
        SystemPage none = dash;
        none.caption = ok ? L"No GPU data" : L"";
        gpu.pages = {none, none, none, none};
    }
    m.push_back(std::move(gpu));

    // Memory: in use -> available
    Metric ram{L"Memory", {}};
    if (ok && s.ramTotal) {
        ram.pages.push_back({Fmt(L"%.0f%%", s.ramUsage), L"", Fmt2(L"%.1f / %.1f GB", s.ramUsed / kGiB, s.ramTotal / kGiB),
                             s.ramUsage / 100.0f, PercentSeverity(s.ramUsage)});
        const double free = static_cast<double>(s.ramTotal - s.ramUsed);
        ram.pages.push_back({Fmt(L"%.1f", free / kGiB), L"GB", L"Available", static_cast<float>(free / static_cast<double>(s.ramTotal)), 0});
    } else {
        ram.pages = {dash, dash};
    }
    m.push_back(std::move(ram));

    // Network: download -> upload (the ring scales to the busiest recent second)
    Metric net{L"Network", {}};
    if (ok) {
        const double scale = NiceCeil(std::max({10e6, static_cast<double>(s.downHistory.Max()), static_cast<double>(s.upHistory.Max())}));
        std::wstring dv, du, uv, uu;
        Rate(s.downBps, dv, du);
        Rate(s.upBps, uv, uu);
        net.pages.push_back({dv, du, L"\u2191 " + RateText(s.upBps), static_cast<float>(std::min(1.0, s.downBps / scale)), 0});
        net.pages.push_back({uv, uu, L"\u2193 " + RateText(s.downBps), static_cast<float>(std::min(1.0, s.upBps / scale)), 0});
    } else {
        net.pages = {dash, dash};
    }
    m.push_back(std::move(net));
    return m;
}

std::wstring RelativeTime(ULONGLONG fileTime) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const ULONGLONG now = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const ULONGLONG secs = now > fileTime ? (now - fileTime) / 10000000ULL : 0;
    if (secs < 60) return L"now";
    if (secs < 3600) return std::to_wstring(secs / 60) + L"m";
    if (secs < 86400) return std::to_wstring(secs / 3600) + L"h";
    return std::to_wstring(secs / 86400) + L"d";
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return {c.r, c.g, c.b, c.a * a}; }

void SetOpenSprings(Spring& w, Spring& h) {
    w.stiffness = 330, w.damping = 30;     // slightly under-damped: a soft, living overshoot
    h.stiffness = 280, h.damping = 27;
}
void SetCloseSprings(Spring& w, Spring& h) {
    w.stiffness = 420, w.damping = 41;     // critically damped: closing never bounces
    h.stiffness = 480, h.damping = 44;
}

const wchar_t kClassName[] = L"Eflow.Island";

} // namespace

IslandWindow::IslandWindow(Gfx& gfx, HMONITOR monitor, HWND owner, std::shared_ptr<const MascotAsset> mascotAsset,
                           MascotRenderer* mascotRenderer)
    : gfx_(gfx), monitor_(monitor), owner_(owner), mascotAsset_(std::move(mascotAsset)), mascotRenderer_(mascotRenderer) {
    if (mascotAsset_ && mascotRenderer_) mascot_ = std::make_unique<Mascot>(mascotAsset_);
    w_.Snap(kCollapsedW);
    h_.Snap(kCollapsedH);
    tabX_.stiffness = tabW_.stiffness = 520;
    tabX_.damping = tabW_.damping = 42;
}

IslandWindow::~IslandWindow() {
    if (watching_ && systemWatchHandler_) systemWatchHandler_(false);
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

bool IslandWindow::Create() {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProcThunk;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                            kClassName, L"Eflow", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    UpdateScale();
    Place();
    if (!CreateSurface()) return false;

    // Selection pill starts on the first tab.
    const auto tabs = TabRects(D2D1::RectF(0, 0, kExpandedW, kExpandedH));
    tabX_.Snap(tabs[0].left);
    tabW_.Snap(tabs[0].right - tabs[0].left);

    SetHitRegion(false);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Animate();
    return true;
}

// ---- geometry --------------------------------------------------------------------------------

void IslandWindow::UpdateScale() {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    scale_ = dx / 96.0f;
}

void IslandWindow::Place() {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_, &mi);
    const RECT& m = mi.rcMonitor;
    pxW_ = static_cast<UINT>(std::ceil((kExpandedW + 2 * kMargin) * scale_));
    pxH_ = static_cast<UINT>(std::ceil((TopInset() + kExpandedH + kMargin) * scale_));
    const int x = m.left + ((m.right - m.left) - static_cast<int>(pxW_)) / 2;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, m.top, static_cast<int>(pxW_), static_cast<int>(pxH_), SWP_NOACTIVATE);
}

D2D1_RECT_F IslandWindow::IslandRect() const {
    const float winW = pxW_ / scale_;
    const float x = (winW - w_.x) / 2.0f;
    return D2D1::RectF(x, TopInset(), x + w_.x, TopInset() + h_.x);
}

float IslandWindow::TopInset() const { return kTopInset; }

float IslandWindow::ContentAlpha() const {
    const float progress = Clamp01((h_.x - kCollapsedH) / (kExpandedH - kCollapsedH));
    return SmoothStep(0.62f, 1.0f, progress);
}

D2D1_POINT_2F IslandWindow::MascotPoint(float x, float y) const {
    // Anchored to the live panel rect, so it rides the open/close springs; it also rises into place
    // as the content fades in, which keeps it in step with the rest of the panel.
    const D2D1_RECT_F r = IslandRect();
    const float cx = r.right - kMascotFromRight;
    const float cy = r.bottom - kMascotFloor + (1.0f - ContentAlpha()) * kMascotRise + kMascotPawsY * kMascotK;
    return {cx + x * kMascotK, cy - y * kMascotK};
}

#ifndef NDEBUG
void IslandWindow::DebugOpenTab(int section) {
    debugPinned_ = section >= 0;
    if (section >= 0) {
        OnClick(section);
        Expand();
    } else if (expanded_) {
        Collapse();
    }
    Animate();
}
#endif

bool IslandWindow::Pinned() const {
    // A pending AI permission request keeps the panel open until it is answered.
    return debugPinned_ || pressed_ >= 0 || (mascot_ && mascot_->permissionPending());
}

std::array<D2D1_RECT_F, kSectionCount> IslandWindow::TabRects(const D2D1_RECT_F& island) const {
    IDWriteTextFormat* fmt = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::array<float, kSectionCount> widths{};
    float total = 0;
    for (int i = 0; i < kSectionCount; ++i) {
        widths[i] = std::ceil(gfx_.MeasureWidth(kTabLabels[i], fmt)) + 2 * kTabPadX;
        total += widths[i];
    }
    total += (kSectionCount - 1) * kTabGap;
    float x = island.left + ((island.right - island.left) - total) / 2.0f;
    std::array<D2D1_RECT_F, kSectionCount> out{};
    for (int i = 0; i < kSectionCount; ++i) {
        out[i] = D2D1::RectF(x, island.top + kTabTop, x + widths[i], island.top + kTabTop + kTabH);
        x += widths[i] + kTabGap;
    }
    return out;
}

bool IslandWindow::CursorInIsland(float slackDip) const {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    const D2D1_POINT_2F p = ToDip(pt);
    const D2D1_RECT_F r = IslandRect();
    return p.x >= r.left - slackDip && p.x <= r.right + slackDip && p.y >= 0 && p.y <= r.bottom + slackDip;
}

void IslandWindow::SetHitRegion(bool full) {
    // A window region makes everything outside it click-through for every process, which keeps the
    // idle island from ever stealing clicks meant for the windows beneath it.
    fullRegion_ = full;
    HRGN rgn;
    if (full) {
        rgn = CreateRectRgn(0, 0, static_cast<int>(pxW_), static_cast<int>(pxH_));
    } else {
        const float winW = pxW_ / scale_;
        const float x0 = (winW - kCollapsedW) / 2.0f - kHotSlackX;
        const float x1 = x0 + kCollapsedW + 2 * kHotSlackX;
        const float y1 = TopInset() + kCollapsedH + kHotSlackY;
        auto px = [&](float v) { return static_cast<int>(std::lround(v * scale_)); };
        rgn = CreateRectRgn(px(x0), 0, px(x1), px(y1));
    }
    SetWindowRgn(hwnd_, rgn, TRUE);   // the system owns rgn from here
}

// ---- state machine ---------------------------------------------------------------------------

void IslandWindow::Expand() {
    if (expanded_ || suppressed_) return;
    expanded_ = true;
    KillTimer(hwnd_, kCloseTimer);
    SetHitRegion(true);
    UpdateSystemWatch();
    SetOpenSprings(w_, h_);
    w_.target = kExpandedW;
    h_.target = kExpandedH;
    Animate();
}

void IslandWindow::Collapse() {
    if (!expanded_) return;
    expanded_ = false;
    hover_ = pressed_ = -1;
    UpdateSystemWatch();
    KillTimer(hwnd_, kAutoCloseTimer);
    autoClosePending_ = false;
    if (mascot_) mascot_->ClearBadge();        // the panel was open: the user has seen it
    SetCloseSprings(w_, h_);
    w_.target = kCollapsedW;
    h_.target = kCollapsedH;
    Animate();
}

void IslandWindow::SetSuppressed(bool suppressed) {
    if (suppressed == suppressed_) return;
    suppressed_ = suppressed;
    if (suppressed) {
        KillTimer(hwnd_, kMascotTimer);       // nothing renders while hidden
        KillTimer(hwnd_, kAutoCloseTimer);
        KillTimer(hwnd_, kOpenTimer);
        KillTimer(hwnd_, kCloseTimer);
        openPending_ = false;
        expanded_ = false;
        hover_ = pressed_ = -1;
        w_.Snap(kCollapsedW);
        h_.Snap(kCollapsedH);
        SetHitRegion(false);
        UpdateSystemWatch();
        ShowWindow(hwnd_, SW_HIDE);
    } else {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Animate();
    }
}

void IslandWindow::UpdateSystemWatch() {
    const bool want = expanded_ && !suppressed_ && section_ == Section::System;
    if (want == watching_) return;
    watching_ = want;
    if (want) {
        ringFrac_.fill(0);                       // rings sweep in from empty each time the tab opens
        ringFade_.fill(1);
    }
    if (systemWatchHandler_) systemWatchHandler_(want);
}

void IslandWindow::SetSystem(const SystemSnapshot& snapshot) {
    system_ = snapshot;
    Animate();
}

void IslandWindow::CyclePage(int metric, int direction) {
    if (metric < 0 || metric >= kMetricCount) return;
    const auto metrics = BuildMetrics(system_);
    const int pages = static_cast<int>(metrics[metric].pages.size());
    if (pages < 2) return;
    ringPage_[metric] = (ringPage_[metric] + direction + pages) % pages;
    Animate();
}

void IslandWindow::SetMascotState(Mascot::State state) {
    // Used by the tray's "Mascot test" menu and scripted tests; real events call the specific setters.
    if (!mascot_) return;
    using State = Mascot::State;
    switch (state) {
    case State::Notify: OnNotification(); break;
    case State::Permission: SetPermissionPending(true); break;
    case State::Music: SetMusicPlaying(true); break;
    default:
        SetPermissionPending(false);
        SetMusicPlaying(false);
        break;
    }
}

void IslandWindow::SetMusicPlaying(bool playing) {
    if (!mascot_) return;
    mascot_->SetAmbient(playing ? Mascot::State::Music : Mascot::State::Idle);
    Animate();
}

void IslandWindow::OnNotification() {
    // Worth attention: open by itself on the Notifications tab, and close again after a moment unless
    // the user has the pointer on it (or it was already open).
    if (mascot_) mascot_->Notify();
    notifScrollTarget_ = 0;                      // newest first: show it
    const bool wasOpen = expanded_;
    OnClick(static_cast<int>(Section::Notifications));
    Expand();
    KillTimer(hwnd_, kAutoCloseTimer);
    autoClosePending_ = !wasOpen;
    if (autoClosePending_) SetTimer(hwnd_, kAutoCloseTimer, kAutoCloseMs, nullptr);
    Animate();
}

void IslandWindow::SetNotifications(const NotificationSnapshot& snapshot) {
    notifications_ = snapshot;
    std::unordered_map<uint32_t, Com<ID2D1Bitmap1>> keep;      // drop icons/hover state of notifications that are gone
    std::unordered_map<uint32_t, float> hover;
    for (const auto& item : notifications_.items) {
        if (auto it = notificationIcons_.find(item.id); it != notificationIcons_.end()) keep[item.id] = std::move(it->second);
        if (auto it = notifCloseHover_.find(item.id); it != notifCloseHover_.end()) hover[item.id] = it->second;
    }
    notificationIcons_ = std::move(keep);
    notifCloseHover_ = std::move(hover);
    const float maxTop = std::max(0.0f, static_cast<float>(notifications_.items.size()) - kNotifRows);
    notifScrollTarget_ = std::clamp(notifScrollTarget_, 0.0f, maxTop);
    hover_ = -1;
    Animate();
}

void IslandWindow::OnWheel(int delta) {
    if (expanded_ && section_ == Section::System && hover_ >= kHitRing && hover_ < kHitRing + kMetricCount) {
        CyclePage(hover_ - kHitRing, delta > 0 ? -1 : +1);          // wheel flips the ring under the pointer
        return;
    }
    if (!expanded_ || section_ != Section::Notifications) return;
    const float maxTop = std::max(0.0f, static_cast<float>(notifications_.items.size()) - kNotifRows);
