#include "island/IslandWindow.h"

#include <d2d1effects.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
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
constexpr float kExpandedW = 620.0f;
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
constexpr int kHitShuffle = 14, kHitRepeat = 15;            // the two extra transport buttons
constexpr int kHitPopBack = 1040;                           // "< Voice": back to the settings list
constexpr int kHitCaption = 17;                             // the live conversation view: click to end it
constexpr int kHitOutput = 16;                              // the "playing on" chip (opens the speaker menu)
constexpr int kHitSeek = 13;                                // the progress bar (click or drag to seek)
constexpr int kHitReject = 20, kHitApprove = 21, kHitDefer = 22;  // permission card hit ids
constexpr int kHitRing = 40;                                 // + metric index (System tab)
constexpr float kMaxPanelH = 344.0f;                        // tallest the panel grows (Solu's menu)
constexpr int kHitAudioButton = 60;                         // + 0 microphone, + 1 speaker (round buttons on the tab row)
constexpr int kHitPopAway = 71;                             // anywhere else on the island while a menu is open: dismiss
constexpr int kHitPopRow = 1000;                            // + visible row in the open device menu
constexpr int kHitPopFooter = 1010;                         // "Sound settings..."
constexpr int kHitPopTest = 1030;                           // the "Test" pill in the microphone / speaker menu
constexpr UINT_PTR kTestTimer = 5;                          // refreshes the menu when a test result expires
constexpr int kHitPopSwitch = 1020;                         // the mute / block switch in the menu header
constexpr float kAudioBtn = 30.0f;                          // round button diameter
constexpr float kPopHeaderH = 30.0f, kPopRowH = 42.0f, kPopFooterH = 38.0f, kMenuPad = 10.0f;   // the device menu, inside the island
constexpr int kPopMaxRows = 4;                              // rows visible at once (the wheel scrolls the rest)
constexpr int kHitNotifClear = 30;                          // "Clear all"
constexpr int kHitNotifClose = 100;                         // + notification index: its dismiss button
constexpr float kNotifRowH = 44.0f;
constexpr float kNotifListTop = 54.0f;                      // from the panel top
constexpr float kNotifRows = 2.0f;                          // rows visible at once
constexpr float kArtSize = 92.0f;
constexpr float kArtTop = 62.0f;
constexpr float kControlsY = 145.0f;       // centre line of the transport buttons (from the panel top)

const wchar_t* const kTabLabels[kSectionCount] = {L"Music", L"Notifications", L"Solu AI", L"AI Requests", L"System"};
const wchar_t* const kEmptyTitle[kSectionCount] = {L"Nothing playing", L"You're all caught up", L"No new messages",
                                                   L"No pending requests", L""};
const wchar_t* const kEmptyBody[kSectionCount] = {
    L"Play something in Spotify, YouTube or your browser",
    L"New notifications will show up here",
    L"WhatsApp messages will show up here",
    L"Claude Code and Codex approvals will appear here",
    L"",
};

std::wstring LowerCase(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}
// ---- System tab: what each ring can show -----------------------------------------------------
constexpr D2D1_COLOR_F kAccent = Rgba(0.42f, 0.70f, 1.0f, 1.0f);
constexpr D2D1_COLOR_F kAmber = Rgba(1.0f, 0.73f, 0.30f, 1.0f);
constexpr D2D1_COLOR_F kRed = Rgba(1.0f, 0.42f, 0.38f, 1.0f);
constexpr D2D1_COLOR_F kGreen = Rgba(0.36f, 0.86f, 0.56f, 1.0f);
constexpr D2D1_COLOR_F kNeutral = Rgba(0.60f, 0.60f, 0.65f, 1.0f);   // "normal": not available, connect it first
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

// Windows names devices "Speakers (High Definition Audio Device)" or "MSI MP223 (NVIDIA High Definition Audio)".
// Show the product on the main line and the kind of device / hardware quietly underneath.
void SplitDeviceName(const std::wstring& full, std::wstring& main, std::wstring& sub) {
    main = full;
    sub.clear();
    const size_t open = full.rfind(L" (");
    if (open == std::wstring::npos || open == 0 || full.back() != L')') return;
    main = full.substr(0, open);
    sub = full.substr(open + 2, full.size() - open - 3);
    static const wchar_t* const kGeneric[] = {L"microphone", L"speakers", L"speaker", L"headphones", L"headset", L"line in",
                                              L"line out", L"stereo mix", L"digital audio", L"digital audio (s/pdif)",
                                              L"output", L"input", L"mic in", L"headset earphone", L"headset microphone"};
    const std::wstring lower = [&] { std::wstring t = main; for (auto& c : t) c = static_cast<wchar_t>(std::towlower(c)); return t; }();
    for (const wchar_t* g : kGeneric)
        if (lower == g) { std::swap(main, sub); return; }
}

const wchar_t* FormGlyph(int form) {          // Segoe Fluent Icons: headphones, monitor, speakers
    switch (form) {
    case 3: case 5: return L"\uE7F6";
    case 9: return L"\uE7F4";
    default: return L"\uE7F5";
    }
}
std::wstring FormLabel(int form, const std::wstring& deviceName) {
    switch (form) {
    case 1: return L"Speakers";
    case 3: return L"Headphones";
    case 5: return L"Headset";
    case 9: return L"Monitor";
    case 8: return L"Digital out";
    case 2: return L"Line out";
    case 0: return L"Network";
    default: {
        std::wstring main, sub;
        SplitDeviceName(deviceName, main, sub);
        return main;
    }
    }
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
    if (audioWatching_ && audioWatchHandler_) audioWatchHandler_(false);
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
    pxH_ = static_cast<UINT>(std::ceil((TopInset() + kMaxPanelH + kMargin) * scale_));
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
    return debugPinned_ || pressed_ >= 0 || (mascot_ && mascot_->permissionPending()) || CaptionsShown() ||
           (soluWake_ && solu_.state == SoluState::Loading);
}

std::array<D2D1_RECT_F, kSectionCount> IslandWindow::TabRects(const D2D1_RECT_F& island) const {
    IDWriteTextFormat* fmt = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::array<float, kSectionCount> text{};
    float textTotal = 0;
    for (int i = 0; i < kSectionCount; ++i) {
        text[i] = std::ceil(gfx_.MeasureWidth(kTabLabels[i], fmt)) + (i == static_cast<int>(Section::Solu) ? 12.0f : 0.0f);   // room for its status dot
        textTotal += text[i];
    }
    // Tabs start at the left; the microphone and speaker buttons sit at the right end of the same row.
    const float left = island.left + 22.0f;
    const float room = (island.right - 22.0f - 4 * kAudioBtn - 3 * 8.0f - 14.0f) - left - (kSectionCount - 1) * kTabGap;
    const float padX = std::clamp((room - textTotal) / (2.0f * kSectionCount), 8.0f, kTabPadX);
    std::array<D2D1_RECT_F, kSectionCount> out{};
    float x = left;
    for (int i = 0; i < kSectionCount; ++i) {
        const float w = text[i] + 2 * padX;
        out[i] = D2D1::RectF(x, island.top + kTabTop, x + w, island.top + kTabTop + kTabH);
        x += w + kTabGap;
    }
    return out;
}

D2D1_RECT_F IslandWindow::AudioButton(const D2D1_RECT_F& island, int side) const {
    const float cx = island.right - 22.0f - kAudioBtn / 2 - (3 - side) * (kAudioBtn + 8.0f);
    const float cy = island.top + kTabTop + kTabH / 2;
    return D2D1::RectF(cx - kAudioBtn / 2, cy - kAudioBtn / 2, cx + kAudioBtn / 2, cy + kAudioBtn / 2);
}

bool IslandWindow::CursorInIsland(float slackDip) const {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    return InsideZone(ToDip(pt), slackDip);
}

bool IslandWindow::InsideZone(D2D1_POINT_2F p, float slack) const {
    const D2D1_RECT_F r = IslandRect();
    return p.x >= r.left - slack && p.x <= r.right + slack && p.y >= 0 && p.y <= r.bottom + slack;
}

void IslandWindow::SetHitRegion(bool full) {
    // A window region makes everything outside it click-through for every process, which keeps the
    // idle island from ever stealing clicks meant for the windows beneath it.
    fullRegion_ = full;
    HRGN rgn;
    if (full) {
        // Only as tall as the panel is (or is heading to): the transparent window below it must stay click-through.
        const float panelH = std::max(h_.target, h_.x);
        const int height = static_cast<int>(std::min<float>(static_cast<float>(pxH_), std::ceil((TopInset() + panelH + kMargin) * scale_)));
        rgn = CreateRectRgn(0, 0, static_cast<int>(pxW_), height);
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
    UpdateSystemWatch();
    UpdateAudioWatch();
    SetOpenSprings(w_, h_);
    w_.target = kExpandedW;
    h_.target = TargetHeight();
    SetHitRegion(true);
    Animate();
}

void IslandWindow::Collapse() {
    if (!expanded_) return;
    expanded_ = false;
    hover_ = pressed_ = -1;
    StopRunningTest();
    popover_ = popoverSide_ = -1;                // the device menu goes with the panel
    popoverT_ = 0;
    UpdateSystemWatch();
    UpdateAudioWatch();
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
        popover_ = popoverSide_ = -1;
        popoverT_ = 0;
        UpdateSystemWatch();
        UpdateAudioWatch();
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

// ---- audio: round buttons on the tab row and the floating device menu ------------------------------

void IslandWindow::UpdateAudioWatch() {
    const bool want = expanded_ && !suppressed_;                // live levels only while the island is open
    if (want == audioWatching_) return;
    audioWatching_ = want;
    if (audioWatchHandler_) audioWatchHandler_(want);
}

int IslandWindow::MaxRows(int side) const { return side == 3 ? 5 : kPopMaxRows; }

void IslandWindow::SetSolu(const SoluSnapshot& snapshot) {
    const bool wasActive = SoluActive();
    const SoluState before = solu_.state;
    solu_ = snapshot;
    if (mascot_) {
        using Talk = Mascot::Talk;
        mascot_->SetTalk(solu_.state == SoluState::Listening ? Talk::Listening : solu_.state == SoluState::Thinking ? Talk::Thinking
                         : solu_.state == SoluState::Speaking ? Talk::Speaking : Talk::None);
        mascot_->SetTalkLevel(solu_.level);
    }
    if (before == SoluState::Loading && solu_.state != SoluState::Loading) soluWake_ = false;
    micImpulse_ = std::max(micImpulse_, solu_.mic);
    if (solu_.mic > 0) {                                               // the microphone meter of the conversation view
        for (size_t i = 1; i < soluBars_.size(); ++i) soluBars_[i - 1] = soluBars_[i];
        soluBars_.back() = Clamp01(solu_.mic);
    }
    if (CaptionsShown()) {
        KillTimer(hwnd_, kCloseTimer);
    } else if ((wasActive || before == SoluState::Loading) && expanded_ && !Pinned() && !CursorInIsland(kLeaveSlack)) {
        SetTimer(hwnd_, kCloseTimer, 1800, nullptr);                   // the conversation is over: tuck away after a moment
    }
    Animate();
}

void IslandWindow::ShowForSolu() {
    if (suppressed_ || !solu_.captions) return;      // captions off: Solu works without opening the island
    soluWake_ = true;
    Expand();                                                          // no-op when already open
    KillTimer(hwnd_, kCloseTimer);
    Animate();
}

void IslandWindow::SetCamera(const CameraSnapshot& snapshot) {
    camera_ = snapshot;
    if (popover_ == 2) ApplyHeightTarget();      // the camera list may have grown or shrunk
    Animate();
}

void IslandWindow::ToggleMute(int side) {
    const bool now = !SideMuted(side);
    if (side == 3) solu_.enabled = !now;                           // optimistic: the service confirms
    else if (side == 0) audio_.input.muted = now;     // show it at once; the service confirms a moment later
    else if (side == 1) audio_.output.muted = now;
    else camera_.blocked = now;
    if (muteHandler_) muteHandler_(side, now);
    Animate();
}

D2D1_RECT_F IslandWindow::MuteSwitch(const PopoverLayout& g) const {
    return D2D1::RectF(g.card.right - 40.0f, g.card.top + 1.0f, g.card.right, g.card.top + 25.0f);
}

void IslandWindow::StopRunningTest() {
    if (audio_.test.state != AudioTest::State::Running) return;
    audio_.test.state = AudioTest::State::Idle;
    if (audioTestHandler_) audioTestHandler_(audio_.test.input, false);
}

void IslandWindow::ToggleTest(int side) {
    if (side < 0 || side > 1) return;
    const bool input = side == 0;
    if (audio_.test.state == AudioTest::State::Running && audio_.test.input == input) {
        StopRunningTest();
    } else {
        audio_.test = AudioTest{};                                    // show it at once; the service takes over
        audio_.test.state = AudioTest::State::Running;
        audio_.test.input = input;
        testBars_.fill(0.0f);
        if (audioTestHandler_) audioTestHandler_(input, true);
    }
    Animate();
}

void IslandWindow::SetAudio(const AudioSnapshot& snapshot) {
    if (snapshot.test.state == AudioTest::State::Running) {           // feed the scrolling meter
        for (size_t i = 1; i < testBars_.size(); ++i) testBars_[i - 1] = testBars_[i];
        testBars_.back() = Clamp01(std::sqrt(snapshot.test.level) * 1.3f);
    } else if (snapshot.test.state == AudioTest::State::Done && snapshot.test.doneAt != lastTestDone_) {
        lastTestDone_ = snapshot.test.doneAt;                         // the verdict is shown for 6 s, then the normal status returns
        SetTimer(hwnd_, kTestTimer, 6100, nullptr);
    }
    audio_ = snapshot;
    if (popover_ >= 0) {                         // keep the open menu valid if a device came or went
        const int total = MenuRows(popover_);
        voiceScroll_ = std::clamp(voiceScroll_, 0, std::max(0, total - MaxRows(popover_)));
        ApplyHeightTarget();                     // the menu may have grown or shrunk
    }
    Animate();
}

void IslandWindow::OpenPopover(int side) {
    if (side != popover_) StopRunningTest();
    settingsPage_ = 0;
    popover_ = side;
    if (side >= 0) popoverSide_ = side;
    voiceScroll_ = 0;
    hover_ = -1;
    ApplyHeightTarget();                         // the island itself grows (or shrinks back) to fit the menu
    Animate();
}

float IslandWindow::MenuHeight(int side) const {
    const int shown = std::min(MenuRows(side), MaxRows(side));
    return kTabTop + kTabH + 8.0f + kPopHeaderH + shown * kPopRowH + kPopFooterH + kMenuPad;
}

IslandWindow::PopoverLayout IslandWindow::PopoverGeometry(const D2D1_RECT_F& island) const {
    PopoverLayout g;
    const int side = popoverSide_ >= 0 ? popoverSide_ : std::max(0, popover_);
    g.total = MenuRows(side);
    g.shown = std::min(g.total, MaxRows(side));
    const float left = island.left + 22.0f;
    const float right = island.left + kExpandedW - (mascot_ ? kMascotLane : 22.0f);
    const float top = island.top + kTabTop + kTabH + 8.0f;
    g.rowsTop = top + kPopHeaderH;
    g.card = D2D1::RectF(left, top, right, g.rowsTop + g.shown * kPopRowH + kPopFooterH);
    g.footer = D2D1::RectF(left, g.rowsTop + g.shown * kPopRowH, right, g.card.bottom);
    return g;
}

void IslandWindow::CyclePage(int metric, int direction) {
    if (metric < 0 || metric >= kMetricCount) return;
    const auto metrics = BuildMetrics(system_);
    const int pages = static_cast<int>(metrics[metric].pages.size());
    if (pages < 2) return;
    ringPage_[metric] = (ringPage_[metric] + direction + pages) % pages;
#ifndef NDEBUG
    Log("system: ring %d -> page %d of %d", metric, ringPage_[metric], pages);
#endif
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
    ApplyHeightTarget();
    Animate();
}

void IslandWindow::OnWheel(int delta) {
    if (expanded_ && section_ == Section::System && hover_ >= kHitRing && hover_ < kHitRing + kMetricCount) {
        CyclePage(hover_ - kHitRing, delta > 0 ? -1 : +1);          // wheel flips the ring under the pointer
        return;
    }
    if (expanded_ && popover_ >= 0) {                                // scroll a long device menu
        const int total = MenuRows(popover_);
        voiceScroll_ = std::clamp(voiceScroll_ + (delta > 0 ? -1 : 1), 0, std::max(0, total - MaxRows(popover_)));
        Animate();
        return;
    }
    if (!expanded_ || section_ != Section::Notifications) return;
    const float maxTop = std::max(0.0f, static_cast<float>(notifications_.items.size()) - kNotifRows);
    notifScrollTarget_ = std::clamp(std::round(notifScrollTarget_) - (delta > 0 ? 1.0f : -1.0f), 0.0f, maxTop);
    Animate();
}

void IslandWindow::SetPermissionPending(bool pending) {
    if (!mascot_) return;
    const bool was = mascot_->permissionPending();
    mascot_->SetPermissionPending(pending);
    if (pending && !was) {                       // open on the AI tab and stay until it is answered
        OnClick(static_cast<int>(Section::Ai));
        Expand();
        KillTimer(hwnd_, kAutoCloseTimer);
        autoClosePending_ = false;
    } else if (!pending && was && expanded_ && !CursorInIsland(kLeaveSlack)) {
        SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);   // answered: let it close
    }
    Animate();
}

int IslandWindow::HitTest(D2D1_POINT_2F p) const {
    if (!expanded_ || h_.x < kExpandedH * 0.85f) return -1;
    const D2D1_RECT_F island = IslandRect();
    const auto tabs = TabRects(island);
    for (int i = 0; i < kSectionCount; ++i)
        if (p.x >= tabs[i].left && p.x <= tabs[i].right && p.y >= tabs[i].top && p.y <= tabs[i].bottom) return i;
    for (int side = 0; side < 4; ++side) {                        // microphone / speaker / camera / settings buttons
        const D2D1_RECT_F b = AudioButton(island, side);
        const float dx = p.x - (b.left + b.right) / 2, dy = p.y - (b.top + b.bottom) / 2;
        if (dx * dx + dy * dy <= (kAudioBtn / 2 + 2) * (kAudioBtn / 2 + 2)) return kHitAudioButton + side;
    }
    if (popover_ >= 0) {                                          // an open menu owns the pointer
        const PopoverLayout g = PopoverGeometry(island);
        auto inside = [&](const D2D1_RECT_F& r) { return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom; };
        for (int i = 0; i < g.shown; ++i)
            if (inside(D2D1::RectF(g.card.left, g.rowsTop + i * kPopRowH, g.card.right, g.rowsTop + (i + 1) * kPopRowH)))
                return kHitPopRow + i;
        if (inside(g.footer)) return kHitPopFooter;
        if (popover_ == 3 && settingsPage_ == 1 && inside(D2D1::RectF(g.card.left - 6, g.card.top - 4, g.card.left + 130, g.card.top + 28))) return kHitPopBack;
        if (popover_ < 2) {
            const D2D1_RECT_F pill = TestPill(g, popover_);
            if (inside(D2D1::RectF(pill.left - 4, pill.top - 4, pill.right + 4, pill.bottom + 4))) return kHitPopTest;
        }
        if (popover_ != 3) {
            D2D1_RECT_F sw = MuteSwitch(g);
            sw = D2D1::RectF(sw.left - 6, sw.top - 4, sw.right + 6, sw.bottom + 4);
            if (inside(sw)) return kHitPopSwitch;
        }
        if (inside(g.card)) return -1;
        return inside(island) ? kHitPopAway : -1;                 // click anywhere else on the island: just dismiss
    }
    if (captionT_ > 0.5f && CaptionsShown()) {                        // the conversation view: a click ends the conversation
        const D2D1_RECT_F c = SoluCaptionRect(island);
        if (p.x >= c.left && p.x <= c.right && p.y >= c.top && p.y <= c.bottom) return kHitCaption;
    }
    if (section_ == Section::System) {
        for (int i = 0; i < kMetricCount; ++i) {
            const D2D1_RECT_F cell = RingCell(island, i);
            if (p.x >= cell.left && p.x <= cell.right && p.y >= cell.top && p.y <= cell.bottom) return kHitRing + i;
        }
    }
    if (section_ == Section::Ai && !permissions_.empty()) {
        const auto buttons = PermissionButtons(island);
        for (int i = 0; i < 3; ++i)
            if (p.x >= buttons[i].left && p.x <= buttons[i].right && p.y >= buttons[i].top && p.y <= buttons[i].bottom)
                return kHitReject + i;
    }
    if (section_ == Section::Notifications && !notifications_.items.empty()) {
        if (const D2D1_RECT_F c = NotificationClear(island); p.x >= c.left && p.x <= c.right && p.y >= c.top && p.y <= c.bottom)
            return kHitNotifClear;
        const D2D1_RECT_F list = NotificationList(island);
        for (size_t i = 0; i < notifications_.items.size(); ++i) {
            const D2D1_RECT_F x = NotificationClose(NotificationRow(island, i));
            if (p.x >= x.left && p.x <= x.right && p.y >= x.top && p.y <= x.bottom && p.y >= list.top && p.y <= list.bottom)
                return kHitNotifClose + static_cast<int>(i);
        }
    }
    if (section_ == Section::Music && media_.hasSession && media_.canSeek && media_.duration > 0) {
        const SeekBar bar = SeekGeometry(island);
        if (p.x >= bar.x0 - 6 && p.x <= bar.x1 + 6 && p.y >= bar.y - 9 && p.y <= bar.y + 10) return kHitSeek;
    }
    if (section_ == Section::Music && media_.hasSession) {
        const auto extra = ExtraControlCenters(island);
        for (int i = 0; i < 2; ++i) {
            const float dx = p.x - extra[i].x, dy = p.y - extra[i].y;
            if (dx * dx + dy * dy <= 18.0f * 18.0f) return kHitShuffle + i;
        }
        if (const OutputInfo out = MediaOutput(); out.valid) {
            const D2D1_RECT_F chip = OutputChipRect(island, out);
            if (p.x >= chip.left && p.x <= chip.right && p.y >= chip.top && p.y <= chip.bottom) return kHitOutput;
        }
        const auto centers = ControlCenters(island);
        for (int i = 0; i < 3; ++i) {
            const float dx = p.x - centers[i].x, dy = p.y - centers[i].y;
            if (dx * dx + dy * dy <= 19.0f * 19.0f) return kHitPrev + i;
        }
    }
    return -1;
}

void IslandWindow::OnMouseMove(D2D1_POINT_2F p) {
    if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        tracking_ = TrackMouseEvent(&tme) != FALSE;
    }
    if (!expanded_) {
        if (!openPending_) {
            openPending_ = true;
            SetTimer(hwnd_, kOpenTimer, kOpenDelayMs, nullptr);
        }
        return;
    }
    const bool inside = InsideZone(p, kLeaveSlack);
    if (inside) KillTimer(hwnd_, kCloseTimer);
    else if (!Pinned()) SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);

    const int hit = HitTest(p);
    const int shown = hit == kHitPopAway ? -1 : hit;               // the dismiss area is not a button: no hand cursor
    if (shown != hover_) {
        hover_ = shown;
        Animate();
    }
}

void IslandWindow::OnClick(int id) {
    if (id == kHitPopAway) {
        OpenPopover(-1);
        return;
    }
    if (id == kHitPopSwitch) {
        if (popover_ >= 0) ToggleMute(popover_);
        return;
    }
    if (id == kHitPopTest) {
        ToggleTest(popover_);
        return;
    }
    if (id == kHitCaption) {                                      // click the conversation view: end the conversation
        if (soluTalk_) soluTalk_();
        return;
    }
    if (id >= kHitAudioButton && id < kHitAudioButton + 4) {      // before the notification ids: their range is open-ended
        const int side = id - kHitAudioButton;
        OpenPopover(popover_ == side ? -1 : side);
        return;
    }
    if (id == kHitPopBack) {                                      // "< Voice": back to the options
        settingsPage_ = 0;
        voiceScroll_ = 0;
        ApplyHeightTarget();
        Animate();
        return;
    }
    if (id == kHitPopFooter && popover_ == 3) return;             // the settings footer is only a hint
    if (id == kHitPopFooter) {
        const wchar_t* page = popover_ == 2 ? L"ms-settings:privacy-webcam" : L"ms-settings:sound";
        OpenPopover(-1);
        ShellExecuteW(nullptr, L"open", page, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id >= kHitPopRow && id < kHitPopRow + 5) {
        if (popover_ == 3) {                                       // the gear's menu
            const int k = voiceScroll_ + (id - kHitPopRow);
            if (settingsPage_ == 0) {
                if (k == 0) {                                      // Solu AI on / off
                    solu_.enabled = !solu_.enabled;
                    if (soluEnable_) soluEnable_(solu_.enabled);
                } else if (k == 1) {                               // live captions
                    solu_.captions = !solu_.captions;
                    if (soluCaptions_) soluCaptions_(solu_.captions);
                } else if (k == 2) {                               // headphones mode
                    solu_.headphones = !solu_.headphones;
                    if (soluHeadphones_) soluHeadphones_(solu_.headphones);
                } else if (k == 3) {                               // Voice >
                    settingsPage_ = 1;
                    voiceScroll_ = 0;
                    ApplyHeightTarget();
                }
            } else if (k >= 0 && k < 5) {                          // a voice: Solu says hello in it
                solu_.voice = kSoluVoices[k].id;
                if (soluVoice_) soluVoice_(solu_.voice);
            }
            hover_ = -1;
            Animate();
            return;
        }
        if (popover_ == 2) return;                                 // cameras are listed, not picked
        if (popover_ >= 0 && audioSelectHandler_) {
            AudioSide& side = popover_ == 0 ? audio_.input : audio_.output;
            const int k = voiceScroll_ + (id - kHitPopRow);
            if (k >= 0 && k < static_cast<int>(side.devices.size())) {
                const std::wstring picked = side.devices[static_cast<size_t>(k)].id;
                for (auto& d : side.devices) d.isDefault = d.id == picked;   // show it now: it becomes the Windows default
                side.name = side.devices[static_cast<size_t>(k)].name;
                side.connected = true;
                side.ready = side.devices[static_cast<size_t>(k)].ready;
                side.hint = side.devices[static_cast<size_t>(k)].hint;
                side.selectedId.clear();
                audioSelectHandler_(popover_ == 0, picked);
            }
        }
        hover_ = -1;                                                       // stay open: the checkmark moves, then Test can be pressed
        Animate();
        return;
    }
    if (id >= kHitRing && id < kHitRing + kMetricCount) {
        CyclePage(id - kHitRing, +1);
        return;
    }
    if (id >= kHitReject && id <= kHitDefer) {
        if (permissions_.empty() || !permissionHandler_) return;
        const PermissionDecision d = id == kHitApprove ? PermissionDecision::Allow
                                   : id == kHitReject  ? PermissionDecision::Deny
                                                       : PermissionDecision::Defer;
        permissionHandler_(permissions_.front().id, d);
        permissions_.erase(permissions_.begin());     // the server confirms with a fresh list right away
        hover_ = -1;
        if (permissions_.empty()) SetPermissionPending(false);
        Animate();
        return;
    }
    if (id == kHitNotifClear || id >= kHitNotifClose) {
        if (!dismissHandler_ || !clearAllHandler_) return;
        if (id == kHitNotifClear) {
            Log("notifications: clear all (button)");
            clearAllHandler_();
            notifications_.items.clear();
        } else if (const size_t i = static_cast<size_t>(id - kHitNotifClose); i < notifications_.items.size()) {
            Log("notifications: dismiss id=%u", notifications_.items[i].id);
            dismissHandler_(notifications_.items[i].id);
            notifications_.items.erase(notifications_.items.begin() + static_cast<std::ptrdiff_t>(i));
        }
        notifScrollTarget_ = std::clamp(notifScrollTarget_, 0.0f,
                                        std::max(0.0f, static_cast<float>(notifications_.items.size()) - kNotifRows));
        hover_ = -1;
        Animate();
        return;
    }
    if (id == kHitShuffle || id == kHitRepeat) {
        if (!mediaHandler_) return;
        if (id == kHitShuffle) {
            if (!media_.canShuffle) return;
            media_.shuffleOn = !media_.shuffleOn;                    // show it now; the app confirms a moment later
            mediaHandler_(MediaCommand::Shuffle, media_.shuffleOn ? 1.0 : 0.0);
        } else {
            if (!media_.canRepeat) return;
            media_.repeat = media_.repeat == 0 ? 2 : media_.repeat == 2 ? 1 : 0;      // off -> list -> track -> off
            mediaHandler_(MediaCommand::Repeat, media_.repeat);
        }
        Animate();
        return;
    }
    if (id == kHitOutput) {                      // the chip is a shortcut to the speaker menu
        OpenPopover(1);
        return;
    }
    if (id == kHitSeek) return;                  // handled by press/drag/release, not as a click
    if (id >= kHitPrev && id <= kHitNext) {
        const bool enabled = id == kHitPlay ? media_.canToggle : id == kHitNext ? media_.canNext : media_.canPrevious;
        if (!enabled || !mediaHandler_) return;
        if (id == kHitPlay) {                    // flip immediately; the service confirms a moment later
            media_.position = media_.PositionNow();
            media_.positionStamp = GetTickCount64();
            media_.playing = !media_.playing;
        }
        mediaHandler_(id == kHitPlay ? MediaCommand::TogglePlayPause : id == kHitNext ? MediaCommand::Next
                                                                                      : MediaCommand::Previous, 0.0);
        Animate();
        return;
    }
    if (id < 0 || id >= kSectionCount) return;
    section_ = static_cast<Section>(id);
    popover_ = -1;                               // switching tabs dismisses an open menu
    UpdateSystemWatch();
    UpdateAudioWatch();
    ApplyHeightTarget();
    const auto tabs = TabRects(D2D1::RectF(0, 0, kExpandedW, kExpandedH));
    tabX_.target = tabs[id].left;
    tabW_.target = tabs[id].right - tabs[id].left;
    Animate();
}

// ---- animation -------------------------------------------------------------------------------

void IslandWindow::Animate() {
    if (frameQueued_ || suppressed_) return;
    frameQueued_ = true;
    KillTimer(hwnd_, kMascotTimer);
    PostMessageW(hwnd_, WM_APP_FRAME, 0, 0);
}

void IslandWindow::Frame() {
    frameQueued_ = false;
#ifndef NDEBUG
    // Debug builds expose a frame counter so tests can verify that a closed, idle island renders nothing.
    SetPropW(hwnd_, L"Eflow.Frames", reinterpret_cast<HANDLE>(reinterpret_cast<UINT_PTR>(GetPropW(hwnd_, L"Eflow.Frames")) + 1));
#endif
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    // The first frame after a pause gets a nominal step instead of the whole gap.
    const float elapsed = static_cast<float>(now.QuadPart - lastFrame_.QuadPart) / freq.QuadPart;
    const float dt = elapsed > 0.25f ? 1.0f / 60 : std::clamp(elapsed, 1.0f / 480, 1.0f / 20);
    lastFrame_ = now;

    const bool moving = StepAnimation(dt);
    const bool mascotVisible = mascot_ && ContentAlpha() > 0.003f;
    if (mascotVisible) mascot_->Update(dt);
    Render();                                   // Present(1) paces this loop to the display refresh
    if (deviceLost_ || suppressed_) return;

    if (moving) {                               // panel animating: run at the display's refresh rate
        frameQueued_ = true;
        PostMessageW(hwnd_, WM_APP_FRAME, 0, 0);
    } else if (mascotVisible) {                 // open and still: only the mascot moves, at a gentler cadence
        SetTimer(hwnd_, kMascotTimer, mascot_->calm() ? kMascotCalmMs : kMascotActiveMs, nullptr);
    }                                           // closed and still: no frames at all
    if (panelMoving_ && !moving) OnSettled();
    panelMoving_ = moving;
}

bool IslandWindow::StepAnimation(float dt) {
    bool moving = false;
    for (Spring* s : {&w_, &h_, &tabX_, &tabW_}) {
        s->Step(dt);
        if (s->AtRest()) s->Snap(s->target);
        else moving = true;
    }
    for (int i = 0; i < 3; ++i) {
        moving |= Approach(controlHover_[i], hover_ == kHitPrev + i ? 1.0f : 0.0f, dt, 22.0f);
        moving |= Approach(permissionHover_[i], hover_ == kHitReject + i ? 1.0f : 0.0f, dt, 22.0f);
    }
    for (int i = 0; i < kSectionCount; ++i) {
        moving |= Approach(tabHover_[i], (i == hover_ && i != static_cast<int>(section_)) ? 1.0f : 0.0f, dt, 22.0f);
        moving |= Approach(sectionAlpha_[i], i == static_cast<int>(section_) ? 1.0f : 0.0f, dt, 16.0f);
    }
    if (expanded_ && section_ == Section::System) {
        const auto metrics = BuildMetrics(system_);
        for (int i = 0; i < kMetricCount; ++i) {
            moving |= Approach(ringHover_[i], hover_ == kHitRing + i ? 1.0f : 0.0f, dt, 20.0f);
            moving |= Approach(ringPress_[i], pressed_ == kHitRing + i ? 1.0f : 0.0f, dt, 30.0f);
            if (ringPage_[i] != ringShown_[i]) {            // fade the centre out, swap, fade the new page in
                Approach(ringFade_[i], 0.0f, dt, 28.0f);
                if (ringFade_[i] < 0.08f) ringShown_[i] = ringPage_[i];
                moving = true;
            } else {
                moving |= Approach(ringFade_[i], 1.0f, dt, 18.0f);
            }
            const auto& pages = metrics[i].pages;
            moving |= Approach(ringFrac_[i], pages[ringPage_[i] % pages.size()].fraction, dt, 6.5f);
        }
    }
    for (int side = 0; side < 4; ++side) {
        moving |= Approach(audioHover_[side], hover_ == kHitAudioButton + side ? 1.0f : 0.0f, dt, 22.0f);
        moving |= Approach(audioOpen_[side], popover_ == side ? 1.0f : 0.0f, dt, 18.0f);
        moving |= Approach(voiceLevel_[side], side < 2 && VoiceSide(side).connected && !SideMuted(side) ? VoiceSide(side).level : 0.0f, dt, 20.0f);
        moving |= Approach(muteAnim_[side], SideMuted(side) ? 0.0f : 1.0f, dt, 18.0f);
    }
    moving |= Approach(popoverT_, popover_ >= 0 ? 1.0f : 0.0f, dt, 16.0f);
    moving |= Approach(captionT_, expanded_ && CaptionsShown() && popover_ < 0 ? 1.0f : 0.0f, dt, 14.0f);
    moving |= Approach(micImpulse_, 0.0f, dt, 6.0f);
    {                                                                                // the thin bar's voice meter
        const float target = std::max(micImpulse_, solu_.state == SoluState::Speaking ? solu_.level : 0.0f);
        moving |= Approach(barLevel_, target, dt, target > barLevel_ ? 28.0f : 8.0f);
        if (SoluAwake() && solu_.state == SoluState::Thinking) moving = true;       // the thinking dots chase each other
    }
    if (captionT_ > 0.01f && solu_.state == SoluState::Speaking) moving = true;     // keep the cat's mouth moving
    if (popover_ < 0 && popoverT_ < 0.003f) popoverT_ = 0, popoverSide_ = -1;
    for (int i = 0; i < static_cast<int>(popRowHover_.size()); ++i)
        moving |= Approach(popRowHover_[i], hover_ == kHitPopRow + i ? 1.0f : 0.0f, dt, 22.0f);
    moving |= Approach(popFooterHover_, hover_ == kHitPopFooter ? 1.0f : 0.0f, dt, 22.0f);
    moving |= Approach(testHover_, hover_ == kHitPopTest ? 1.0f : 0.0f, dt, 22.0f);
    for (int i = 0; i < 3; ++i) moving |= Approach(extraHover_[i], hover_ == (i == 2 ? kHitOutput : kHitShuffle + i) ? 1.0f : 0.0f, dt, 22.0f);
    moving |= Approach(seekHover_, (seeking_ || hover_ == kHitSeek) ? 1.0f : 0.0f, dt, 22.0f);
    moving |= Approach(notifScroll_, notifScrollTarget_, dt, 16.0f);
    moving |= Approach(notifClearHover_, hover_ == kHitNotifClear ? 1.0f : 0.0f, dt, 22.0f);
    for (size_t i = 0; i < notifications_.items.size(); ++i)
        moving |= Approach(notifCloseHover_[notifications_.items[i].id],
                           hover_ == kHitNotifClose + static_cast<int>(i) ? 1.0f : 0.0f, dt, 22.0f);
    return moving;
}

void IslandWindow::OnSettled() {
    if (expanded_) {
        SetHitRegion(true);                      // the panel may have changed height: fit the clickable area to it
        // Opened by hover but the pointer already left: start closing. (Self-opened panels have their own timer.)
        if (!CursorInIsland(kLeaveSlack) && !Pinned() && !autoClosePending_) SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);
    } else if (fullRegion_) {
        SetHitRegion(false);
    }
}

// ---- rendering -------------------------------------------------------------------------------

bool IslandWindow::CreateSurface() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = pxW_;
    desc.Height = pxH_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(gfx_.dxgiFactory()->CreateSwapChainForComposition(gfx_.d3d(), &desc, nullptr, &swap_))) return false;

    if (FAILED(gfx_.dcomp()->CreateTargetForHwnd(hwnd_, TRUE, &target_))) return false;
    if (FAILED(gfx_.dcomp()->CreateVisual(&visual_))) return false;
    visual_->SetContent(swap_.Get());
    target_->SetRoot(visual_.Get());
    if (FAILED(gfx_.dcomp()->Commit())) return false;

    if (FAILED(gfx_.d2dDevice()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    Com<IDXGISurface> surface;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f * scale_, 96.0f * scale_);
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &backbuffer_))) return false;
    Com<ID3D11Texture2D> backTexture;              // the same back buffer, for the mascot's Direct3D pass
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&backTexture))) ||
        FAILED(gfx_.d3d()->CreateRenderTargetView(backTexture.Get(), nullptr, &rtv_)))
        return false;
    dc_->SetTarget(backbuffer_.Get());
    dc_->SetDpi(96.0f * scale_, 96.0f * scale_);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType is wrong on transparent targets
    if (FAILED(dc_->CreateSolidColorBrush(kText, &brush_))) return false;
    if (FAILED(dc_->CreateEffect(CLSID_D2D1Shadow, &shadow_))) return false;
    return true;
}

void IslandWindow::Render() {
    if (!dc_ || deviceLost_) return;
    const float progress = Clamp01((h_.x - kCollapsedH) / (kExpandedH - kCollapsedH));

    // Pass 1 (Direct2D): panel. Pass 2 (Direct3D): mascot on top. Pass 3 (Direct2D): mascot effects.
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (!suppressed_) DrawIsland(dc_.Get(), progress);
    HRESULT hr = dc_->EndDraw();
    const float mascotAlpha = mascot_ && !suppressed_ ? ContentAlpha() : 0.0f;
    if (SUCCEEDED(hr) && mascotAlpha > 0.003f) {
        DrawMascot(mascotAlpha);
        dc_->BeginDraw();
        DrawMascotEffects(dc_.Get(), mascotAlpha);
        hr = dc_->EndDraw();
    }
    if (SUCCEEDED(hr)) hr = swap_->Present(1, 0);
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        deviceLost_ = true;
        PostMessageW(owner_, WM_APP_DEVICE_LOST, 0, 0);
    }
}

void IslandWindow::DrawShadow(ID2D1DeviceContext* dc, D2D1_ROUNDED_RECT shape, float progress) {
    Com<ID2D1CommandList> list;
    if (FAILED(dc->CreateCommandList(&list))) return;
    Com<ID2D1Image> previous;
    dc->GetTarget(&previous);
    dc->SetTarget(list.Get());
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 1));
    dc->FillRoundedRectangle(shape, brush_.Get());
    dc->SetTarget(previous.Get());
    list->Close();

    shadow_->SetInput(0, list.Get());
    shadow_->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 9.0f + 5.0f * progress);
    shadow_->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.42f * progress));
    dc->DrawImage(shadow_.Get(), D2D1::Point2F(0, 4.0f + 4.0f * progress));
}

void IslandWindow::DrawIsland(ID2D1DeviceContext* dc, float progress) {
    const D2D1_RECT_F r = IslandRect();
    const float radius = std::min(kMaxRadius, (r.bottom - r.top) / 2.0f);
    const D2D1_ROUNDED_RECT shape{r, radius, radius};

    if (progress > 0.01f) DrawShadow(dc, shape, progress);

    // Idle bar is a little translucent so it reads as part of the desktop, not a widget.
    brush_->SetColor(WithAlpha(kIslandFill, 0.80f + 0.18f * progress));
    dc->FillRoundedRectangle(shape, brush_.Get());

    // Hairline edge: barely there, gives the dark surface a crisp silhouette on dark wallpapers.
    const D2D1_ROUNDED_RECT edge{D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
                                 std::max(0.0f, radius - 0.5f), std::max(0.0f, radius - 0.5f)};
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f + 0.04f * progress));
    dc->DrawRoundedRectangle(edge, brush_.Get(), 1.0f);

    if (progress < 0.34f) DrawSoluBar(dc, r, 1.0f - progress / 0.34f);           // Solu is awake: the bar shows it
    const float contentAlpha = SmoothStep(0.62f, 1.0f, progress);
    if (contentAlpha > 0.003f) {
        dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawContent(dc, r, contentAlpha);
        dc->PopAxisAlignedClip();
    }
}

void IslandWindow::DrawText(ID2D1DeviceContext* dc, const wchar_t* text, IDWriteTextFormat* fmt, D2D1_RECT_F r,
                            D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT vertical) {
    if (!fmt) return;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(vertical);
    brush_->SetColor(color);
    dc->DrawTextW(text, static_cast<UINT32>(wcslen(text)), fmt, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void IslandWindow::DrawContent(ID2D1DeviceContext* dc, D2D1_RECT_F island, float alpha) {
    // Content drifts up slightly as it fades in, so the panel "settles" rather than pops.
    const float drift = (1.0f - alpha) * 6.0f;
    const D2D1_RECT_F base = D2D1::RectF(island.left, island.top + drift, island.left + kExpandedW, island.top + kExpandedH + drift);
    const auto tabs = TabRects(base);

    // Selection pill.
    const float pillTop = base.top + kTabTop;
    const D2D1_ROUNDED_RECT pill{D2D1::RectF(base.left + tabX_.x, pillTop, base.left + tabX_.x + tabW_.x, pillTop + kTabH),
                                 kTabH / 2, kTabH / 2};
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.11f * alpha));
    dc->FillRoundedRectangle(pill, brush_.Get());

    IDWriteTextFormat* tabFont = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    for (int i = 0; i < kSectionCount; ++i) {
        if (tabHover_[i] > 0.0f) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.055f * tabHover_[i] * alpha));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{tabs[i], kTabH / 2, kTabH / 2}, brush_.Get());
        }
        const float selected = sectionAlpha_[i];
        const float a = 0.50f + 0.45f * selected + 0.25f * tabHover_[i];
        D2D1_RECT_F labelRect = tabs[i];
        if (i == static_cast<int>(Section::Solu)) {                  // a status dot: green awake, amber busy, none when off
            labelRect.left += 12.0f;
            if (SoluAwake() || solu_.state == SoluState::Loading) {
                const bool busy = solu_.state == SoluState::Loading || solu_.state == SoluState::Thinking;
                const D2D1_COLOR_F c = busy ? kAmber : kGreen;
                brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, std::min(a + 0.2f, 1.0f) * alpha));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tabs[i].left + 12.0f, (tabs[i].top + tabs[i].bottom) / 2), 3.0f, 3.0f), brush_.Get());
            }
        }
        DrawText(dc, kTabLabels[i], tabFont, labelRect, WithAlpha(kText, std::min(a, 0.96f) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    DrawAudioButtons(dc, base, alpha);

    // Section body: empty states for now; cross-fades with a small vertical slide.
    IDWriteTextFormat* titleFont = gfx_.Text(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* bodyFont = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    const float bodyTop = base.top + kTabTop + kTabH + 18.0f;
    const float bodyBottom = base.bottom - 18.0f;
    const float mid = (bodyTop + bodyBottom) / 2.0f;
    for (int i = 0; i < kSectionCount; ++i) {
        const float a = sectionAlpha_[i] * alpha * (1.0f - popoverT_) * (1.0f - captionT_);   // ... to the menu or the conversation
        if (a < 0.003f) continue;
        const float slide = (1.0f - sectionAlpha_[i]) * 8.0f;
        if (i == static_cast<int>(Section::System)) {
            DrawSystem(dc, D2D1::RectF(base.left, base.top + slide, base.right, base.bottom + slide), a);
            continue;
        }
        if (i == static_cast<int>(Section::Ai) && !permissions_.empty()) {
            DrawPermission(dc, D2D1::RectF(base.left, base.top + slide, base.right, base.bottom + slide), a);
            continue;
        }
        if (i == static_cast<int>(Section::Solu)) {
            DrawSolu(dc, D2D1::RectF(base.left, base.top + slide, base.right, base.bottom + slide), a);
            continue;
        }
        if (i == static_cast<int>(Section::Notifications) && !notifications_.items.empty()) {
            DrawNotifications(dc, D2D1::RectF(base.left, base.top + slide, base.right, base.bottom + slide), a);
            continue;
        }
        if (i == static_cast<int>(Section::Music) && media_.hasSession) {
            DrawMusic(dc, D2D1::RectF(base.left, base.top + slide, base.right, base.bottom + slide), a);
            continue;
        }
        const float right = base.right - (mascot_ ? kMascotLane : 24.0f);
        const wchar_t* emptyTitle = kEmptyTitle[i];
        const wchar_t* emptyBody = kEmptyBody[i];
        if (i == static_cast<int>(Section::Notifications)) {
            switch (notifications_.access) {
            case NotificationAccess::Denied:
                emptyTitle = L"Notification access is off";
                emptyBody = L"Allow it in Settings \u203A Privacy \u203A Notifications";
                break;
            case NotificationAccess::Unsupported:
                emptyTitle = L"Notifications are unavailable";
                emptyBody = L"Eflow needs to be installed to read them";
                break;
            case NotificationAccess::Unknown:
                emptyTitle = L"Waiting for permission";
                emptyBody = L"Choose Allow when Windows asks";
                break;
            default: break;
            }
        }
        DrawText(dc, emptyTitle, titleFont, D2D1::RectF(base.left + 24, mid - 26 + slide, right, mid + slide),
                 WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, emptyBody, bodyFont, D2D1::RectF(base.left + 24, mid + 2 + slide, right, mid + 24 + slide),
                 WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    DrawSoluCaption(dc, base, alpha);
    DrawAudioPopover(dc, base, alpha);
}

// ---- music -----------------------------------------------------------------------------------

IslandWindow::SeekBar IslandWindow::SeekGeometry(const D2D1_RECT_F& panel) const {
    // The same numbers DrawMusic uses: left edge after the artwork, right edge before the mascot's lane.
    return {panel.left + 24 + kArtSize + 18, panel.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f), panel.top + 113 + 1.5f};
}

float IslandWindow::SeekFractionAt(float x, const D2D1_RECT_F& panel) const {
    const SeekBar bar = SeekGeometry(panel);
    return Clamp01((x - bar.x0) / std::max(1.0f, bar.x1 - bar.x0));
}

void IslandWindow::SetMedia(const MediaInfo& media) {
    media_ = media;
    Animate();
}

std::array<D2D1_POINT_2F, 2> IslandWindow::ExtraControlCenters(const D2D1_RECT_F& island) const {
    const auto c = ControlCenters(island);
    return {D2D1::Point2F(c[0].x - 46, c[0].y), D2D1::Point2F(c[2].x + 46, c[2].y)};
}

IslandWindow::OutputInfo IslandWindow::MediaOutput() const {
    OutputInfo out;
    if (!audio_.valid || audio_.output.devices.empty()) return out;
    const AudioDevice* shown = nullptr;
    const std::wstring app = LowerCase(media_.sourceApp);
    if (!app.empty()) {                          // the device that has an active stream from the playing app
        for (const auto& d : audio_.output.devices)
            for (const auto& a : d.apps)
                if (LowerCase(a) == app || LowerCase(a).find(app) != std::wstring::npos) shown = &d, out.playingHere = true;
    }
    if (!shown) {                                // otherwise it will come out of the Windows default
        for (const auto& d : audio_.output.devices)
            if (d.isDefault) shown = &d;
    }
    if (!shown) return out;
    out.valid = true;
    out.glyph = FormGlyph(shown->form);
    out.label = FormLabel(shown->form, shown->name);
    return out;
}

D2D1_RECT_F IslandWindow::OutputChipRect(const D2D1_RECT_F& base, const OutputInfo& out) const {
    const float x1 = base.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    const float width = 8.0f + 14.0f + 4.0f + std::ceil(gfx_.MeasureWidth(out.label.c_str(), gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD))) + 8.0f;
    return D2D1::RectF(x1 - width, base.top + 83.5f, x1, base.top + 103.5f);
}

std::array<D2D1_POINT_2F, 3> IslandWindow::ControlCenters(const D2D1_RECT_F& island) const {
    const float x0 = island.left + 24 + kArtSize + 18;
    const float x1 = island.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    const float cx = (x0 + x1) / 2.0f, cy = island.top + kControlsY;
    return {D2D1::Point2F(cx - 46, cy), D2D1::Point2F(cx, cy), D2D1::Point2F(cx + 46, cy)};
}

void IslandWindow::UpdateArtwork() {
    if (artworkId_ == media_.artworkId) return;
    artworkId_ = media_.artworkId;
    artwork_.Reset();
    const auto& img = media_.artwork;
    if (!img || img->empty()) return;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc_->CreateBitmap(D2D1::SizeU(img->width, img->height), img->pixels.data(), img->width * 4, &props, &artwork_);
}

namespace {
std::wstring FormatTime(double seconds) {
    const int s = static_cast<int>(seconds + 0.5);
    wchar_t buf[16];
    if (s >= 3600) swprintf_s(buf, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else swprintf_s(buf, L"%d:%02d", s / 60, s % 60);
    return buf;
}
} // namespace

void IslandWindow::DrawMusic(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    UpdateArtwork();

    // Artwork, cover-fitted into a rounded square.
    const D2D1_RECT_F art = D2D1::RectF(base.left + 24, base.top + kArtTop, base.left + 24 + kArtSize,
                                        base.top + kArtTop + kArtSize);
    const D2D1_ROUNDED_RECT artShape{art, 12, 12};
    if (artwork_) {
        const D2D1_SIZE_F sz = artwork_->GetSize();
        const float s = std::max(kArtSize / sz.width, kArtSize / sz.height);
        Com<ID2D1BitmapBrush1> fill;
        const auto brushProps = D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                             D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        if (SUCCEEDED(dc->CreateBitmapBrush(artwork_.Get(), &brushProps, nullptr, &fill))) {
            fill->SetTransform(D2D1::Matrix3x2F::Scale(s, s) *
                               D2D1::Matrix3x2F::Translation(art.left + (kArtSize - sz.width * s) / 2,
                                                             art.top + (kArtSize - sz.height * s) / 2));
            fill->SetOpacity(alpha);
            dc->FillRoundedRectangle(artShape, fill.Get());
        }
    } else {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.07f * alpha));
        dc->FillRoundedRectangle(artShape, brush_.Get());
        DrawText(dc, L"\u266A", gfx_.Text(30.0f, DWRITE_FONT_WEIGHT_NORMAL), art, WithAlpha(kTextDim, alpha),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * alpha));          // hairline keeps dark covers crisp
    dc->DrawRoundedRectangle(artShape, brush_.Get(), 1.0f);

    // Text column.
    const float x0 = art.right + 18;
    const float x1 = base.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    const std::wstring title = media_.title.empty() ? L"Unknown title" : media_.title;
    std::wstring subtitle = media_.artist;
    if (!media_.sourceApp.empty()) subtitle += (subtitle.empty() ? L"" : L"  \u00B7  ") + media_.sourceApp;
    DrawText(dc, title.c_str(), gfx_.Text(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             D2D1::RectF(x0, base.top + 64, x1, base.top + 84), WithAlpha(kText, alpha));
    float subtitleRight = x1;
    if (const OutputInfo out = MediaOutput(); out.valid) {                    // where the sound comes out: headphones, speakers...
        const D2D1_RECT_F chip = OutputChipRect(base, out);
        subtitleRight = chip.left - 8.0f;
        const float h = extraHover_[2];
        const bool live = media_.playing && out.playingHere;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.07f + 0.07f * h) * alpha));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{chip, 10, 10}, brush_.Get());
        DrawText(dc, out.glyph, gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(chip.left + 6, chip.top, chip.left + 22, chip.bottom),
                 WithAlpha(live ? kGreen : kText, (live ? 0.95f : 0.62f) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, out.label.c_str(), gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(chip.left + 26, chip.top, chip.right - 4, chip.bottom),
                 WithAlpha(kText, (0.72f + 0.2f * h) * alpha));
    }
    DrawText(dc, subtitle.c_str(), gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL),
             D2D1::RectF(x0, base.top + 84, subtitleRight, base.top + 102), WithAlpha(kTextDim, alpha));

    // Progress. When the app allows seeking the bar thickens on hover and shows a handle you can drag.
    const float barY = base.top + 113;
    const bool seekable = media_.canSeek && media_.duration > 0;
    const float grow = seekable ? seekHover_ : 0.0f;
    const float thick = 3.0f + 2.5f * grow, top = barY + 1.5f - thick / 2;
    const D2D1_ROUNDED_RECT track{D2D1::RectF(x0, top, x1, top + thick), thick / 2, thick / 2};
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.14f + 0.05f * grow) * alpha));
    dc->FillRoundedRectangle(track, brush_.Get());
    const double pos = seeking_ ? seekFraction_ * media_.duration : media_.PositionNow();
    if (media_.duration > 0) {
        const float f = static_cast<float>(std::clamp(pos / media_.duration, 0.0, 1.0));
        const float fillX = x0 + (x1 - x0) * f;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.88f + 0.1f * grow) * alpha));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, top, std::max(x0 + thick, fillX), top + thick), thick / 2, thick / 2},
                                 brush_.Get());
        if (grow > 0.01f) {                                          // the handle
            const float r = 5.5f * grow;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillX, barY + 1.5f), r, r), brush_.Get());
        }
        IDWriteTextFormat* timeFont = gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL);
        DrawText(dc, FormatTime(pos).c_str(), timeFont, D2D1::RectF(x0, barY + 5, x0 + 60, barY + 19),
                 seeking_ ? WithAlpha(kText, alpha) : WithAlpha(kTextDim, alpha));
        DrawText(dc, FormatTime(media_.duration).c_str(), timeFont, D2D1::RectF(x1 - 60, barY + 5, x1, barY + 19),
                 WithAlpha(kTextDim, alpha), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Transport controls (Segoe Fluent Icons): previous, play/pause, next.
    const auto centers = ControlCenters(D2D1::RectF(base.left, base.top, base.right, base.bottom));
    const wchar_t* glyphs[3] = {L"\uE892", media_.playing ? L"\uE769" : L"\uE768", L"\uE893"};
    const bool enabled[3] = {media_.canPrevious, media_.canToggle, media_.canNext};
    IDWriteTextFormat* iconFont = gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_NORMAL, true);
    for (int i = 0; i < 3; ++i) {
        const D2D1_POINT_2F c = centers[i];
        const float r = i == 1 ? 17.0f : 15.0f;
        const float hoverA = enabled[i] ? controlHover_[i] : 0.0f;
        if (i == 1) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.10f + 0.08f * hoverA) * alpha));
            dc->FillEllipse(D2D1::Ellipse(c, r, r), brush_.Get());
        } else if (hoverA > 0) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * hoverA * alpha));
            dc->FillEllipse(D2D1::Ellipse(c, r, r), brush_.Get());
        }
        DrawText(dc, glyphs[i], iconFont, D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r),
                 WithAlpha(kText, (enabled[i] ? 0.95f : 0.28f) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // Shuffle and repeat flank the transport buttons: green when on, with a small dot under them.
    const auto extra = ExtraControlCenters(D2D1::RectF(base.left, base.top, base.right, base.bottom));
    const wchar_t* extraGlyphs[2] = {L"", media_.repeat == 1 ? L"" : L""};
    const bool extraEnabled[2] = {media_.canShuffle, media_.canRepeat};
    const bool extraOn[2] = {media_.shuffleOn, media_.repeat != 0};
    IDWriteTextFormat* smallIcons = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_NORMAL, true);
    for (int i = 0; i < 2; ++i) {
        const D2D1_POINT_2F c = extra[i];
        const float hoverA = extraEnabled[i] ? extraHover_[i] : 0.0f;
        if (hoverA > 0) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * hoverA * alpha));
            dc->FillEllipse(D2D1::Ellipse(c, 15, 15), brush_.Get());
        }
        const D2D1_COLOR_F tint = extraOn[i] ? kGreen : kText;
        DrawText(dc, extraGlyphs[i], smallIcons, D2D1::RectF(c.x - 15, c.y - 15, c.x + 15, c.y + 15),
                 WithAlpha(tint, (!extraEnabled[i] ? 0.22f : extraOn[i] ? 0.98f : 0.55f + 0.3f * hoverA) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (extraOn[i] && extraEnabled[i]) {
            brush_->SetColor(D2D1::ColorF(kGreen.r, kGreen.g, kGreen.b, 0.95f * alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y + 13), 1.7f, 1.7f), brush_.Get());
        }
    }
}

// ---- AI permission requests -------------------------------------------------------------------

void IslandWindow::SetPermissions(const std::vector<PermissionRequest>& pending) {
    permissions_ = pending;
    SetPermissionPending(!permissions_.empty());   // opens on the AI tab for a new request; closes when done
    Animate();
}

std::array<D2D1_RECT_F, 3> IslandWindow::PermissionButtons(const D2D1_RECT_F& island) const {
    const float x0 = island.left + 24;
    const float x1 = island.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    const float top = island.top + 136, bottom = top + 28;
    const D2D1_RECT_F approve = D2D1::RectF(x1 - 96, top, x1, bottom);
    const D2D1_RECT_F reject = D2D1::RectF(approve.left - 8 - 84, top, approve.left - 8, bottom);
    const D2D1_RECT_F defer = D2D1::RectF(x0, top, reject.left - 8, bottom);
    return {reject, approve, defer};
}

D2D1_RECT_F IslandWindow::NotificationList(const D2D1_RECT_F& island) const {
    return D2D1::RectF(island.left + 18, island.top + kNotifListTop,
                       island.left + kExpandedW - (mascot_ ? kMascotLane : 18.0f), island.top + kNotifListTop + kNotifRows * kNotifRowH);
}

D2D1_RECT_F IslandWindow::NotificationRow(const D2D1_RECT_F& island, size_t index) const {
    const D2D1_RECT_F list = NotificationList(island);
    const float top = list.top + (static_cast<float>(index) - notifScroll_) * kNotifRowH;
    return D2D1::RectF(list.left, top, list.right, top + kNotifRowH);
}

D2D1_RECT_F IslandWindow::NotificationClose(const D2D1_RECT_F& row) const {
    const float cy = row.top + kNotifRowH / 2;
    return D2D1::RectF(row.right - 28, cy - 11, row.right - 6, cy + 11);
}

D2D1_RECT_F IslandWindow::NotificationClear(const D2D1_RECT_F& island) const {
    const D2D1_RECT_F list = NotificationList(island);
    return D2D1::RectF(list.right - 80, list.bottom + 2, list.right, list.bottom + 18);
}

void IslandWindow::DrawNotifications(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    const D2D1_RECT_F list = NotificationList(base);
    IDWriteTextFormat* titleFont = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* bodyFont = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* metaFont = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL);

    dc->PushAxisAlignedClip(D2D1::RectF(list.left, list.top, list.right, list.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < notifications_.items.size(); ++i) {
        const D2D1_RECT_F row = NotificationRow(base, i);
        if (row.bottom < list.top || row.top > list.bottom) continue;
        const NotificationItem& n = notifications_.items[i];

        // App logo, or an initial on a tinted tile when the app provides none.
        const D2D1_RECT_F icon = D2D1::RectF(row.left + 4, row.top + 8, row.left + 32, row.top + 36);
        auto& bitmap = notificationIcons_[n.id];
        if (!bitmap && n.icon && !n.icon->empty()) {
            const auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc_->CreateBitmap(D2D1::SizeU(n.icon->width, n.icon->height), n.icon->pixels.data(), n.icon->width * 4, &props, &bitmap);
        }
        if (bitmap) {
            dc->DrawBitmap(bitmap.Get(), icon, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else {
            // A stable tint per app (hash of its name) so the same app always looks the same.
            uint32_t hash = 2166136261u;
            for (wchar_t ch : n.app) hash = (hash ^ static_cast<uint32_t>(ch)) * 16777619u;
            const float hue = static_cast<float>(hash % 360) / 60.0f;
            const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
            const float rgb[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
            const float* t = rgb[static_cast<int>(hue) % 6];
            brush_->SetColor(D2D1::ColorF(0.18f + 0.30f * t[0], 0.18f + 0.30f * t[1], 0.18f + 0.30f * t[2], alpha));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{icon, 7, 7}, brush_.Get());
            const wchar_t initial[2] = {n.app.empty() ? L'?' : n.app[0], 0};
            DrawText(dc, initial, titleFont, icon, WithAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const auto hoverIt = notifCloseHover_.find(n.id);
        const float hover = hoverIt != notifCloseHover_.end() ? hoverIt->second : 0.0f;
        const D2D1_RECT_F close = NotificationClose(row);
        const std::wstring meta = (n.app.empty() ? std::wstring() : n.app + L"  \u00B7  ") + RelativeTime(n.time);
        const float textLeft = icon.right + 12;
        const float metaW = std::min(std::ceil(gfx_.MeasureWidth(meta.c_str(), metaFont)) + 2, 150.0f);
        const float textRight = close.left - 4;
        DrawText(dc, n.title.empty() ? n.app.c_str() : n.title.c_str(), titleFont,
                 D2D1::RectF(textLeft, row.top + 6, std::max(textLeft, textRight - metaW - 8), row.top + 24), WithAlpha(kText, alpha));
        DrawText(dc, meta.c_str(), metaFont, D2D1::RectF(textRight - metaW, row.top + 6, textRight, row.top + 24),
                 WithAlpha(kTextDim, alpha), DWRITE_TEXT_ALIGNMENT_TRAILING);
        std::wstring body = n.body;
        std::replace(body.begin(), body.end(), L'\n', L' ');
        DrawText(dc, body.c_str(), bodyFont, D2D1::RectF(textLeft, row.top + 23, textRight, row.top + 40),
                 WithAlpha(kTextDim, 1.15f * alpha));

        // Dismiss: faint until the pointer is on it.
        const D2D1_POINT_2F c = D2D1::Point2F((close.left + close.right) / 2, (close.top + close.bottom) / 2);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * hover * alpha));
        dc->FillEllipse(D2D1::Ellipse(c, 10, 10), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.32f + 0.5f * hover) * alpha));
        dc->DrawLine(D2D1::Point2F(c.x - 3.5f, c.y - 3.5f), D2D1::Point2F(c.x + 3.5f, c.y + 3.5f), brush_.Get(), 1.4f);
        dc->DrawLine(D2D1::Point2F(c.x - 3.5f, c.y + 3.5f), D2D1::Point2F(c.x + 3.5f, c.y - 3.5f), brush_.Get(), 1.4f);
    }
    dc->PopAxisAlignedClip();

    // Footer: count on the left, "Clear all" on the right.
    const size_t total = notifications_.items.size();
    const std::wstring count = std::to_wstring(total) + (total == 1 ? L" notification" : L" notifications");
    DrawText(dc, count.c_str(), metaFont, D2D1::RectF(list.left + 4, list.bottom + 2, list.left + 200, list.bottom + 18),
             WithAlpha(kTextDim, 0.8f * alpha));
    DrawText(dc, L"Clear all", metaFont, NotificationClear(base), WithAlpha(kText, (0.55f + 0.4f * notifClearHover_) * alpha),
             DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void IslandWindow::DrawPermission(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    const PermissionRequest& r = permissions_.front();
    const float x0 = base.left + 24;
    const float x1 = base.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);

    // "<Source> wants to <action>" with the source in semibold.
    IDWriteTextFormat* strong = gfx_.Text(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* normal = gfx_.Text(13.5f, DWRITE_FONT_WEIGHT_NORMAL);
    const float sourceW = std::ceil(gfx_.MeasureWidth(r.source.c_str(), strong));
    DrawText(dc, r.source.c_str(), strong, D2D1::RectF(x0, base.top + 58, x0 + sourceW + 1, base.top + 76),
             WithAlpha(kText, alpha));
    const std::wstring rest = L" wants to " + r.action;
    DrawText(dc, rest.c_str(), normal, D2D1::RectF(x0 + sourceW, base.top + 58, x1, base.top + 76),
             WithAlpha(kText, 0.85f * alpha));

    // Project, queue position, and a warning for destructive-looking commands.
    std::wstring meta = r.project.empty() ? std::wstring() : L"in " + r.project;
    if (permissions_.size() > 1)
        meta += (meta.empty() ? L"" : L"  \u00B7  ") + std::wstring(L"1 of ") + std::to_wstring(permissions_.size());
    IDWriteTextFormat* smallFont = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    const float metaW = std::ceil(gfx_.MeasureWidth(meta.c_str(), smallFont));
    DrawText(dc, meta.c_str(), smallFont, D2D1::RectF(x0, base.top + 76, x1, base.top + 92), WithAlpha(kTextDim, alpha));
    const D2D1_COLOR_F warn = D2D1::ColorF(1.0f, 0.42f, 0.38f, alpha);
    if (r.risky)
        DrawText(dc, meta.empty() ? L"Potentially destructive" : L"  \u00B7  Potentially destructive", smallFont,
                 D2D1::RectF(x0 + metaW, base.top + 76, x1, base.top + 92), warn);

    // The exact command / path, verbatim, in a monospace box (two lines, then an ellipsis).
    const D2D1_RECT_F box = D2D1::RectF(x0, base.top + 96, x1, base.top + 130);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * alpha));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{box, 8, 8}, brush_.Get());
    if (r.risky) {
        brush_->SetColor(D2D1::ColorF(1.0f, 0.42f, 0.38f, 0.55f * alpha));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{box, 8, 8}, brush_.Get(), 1.0f);
    }
    const std::wstring detail = r.detail.empty() ? r.tool : r.detail;
    DrawText(dc, detail.c_str(), gfx_.Mono(11.0f), D2D1::RectF(box.left + 10, box.top + 3, box.right - 10, box.bottom - 3),
             WithAlpha(kText, 0.92f * alpha));

    // Buttons: Reject, Approve, and a quiet way to answer in the tool itself instead.
    const auto buttons = PermissionButtons(base);
    const float hr = permissionHover_[0], ha = permissionHover_[1], hd = permissionHover_[2];
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.10f + 0.06f * hr) * alpha));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{buttons[0], 14, 14}, brush_.Get());
    brush_->SetColor(D2D1::ColorF(0.04f + 0.06f * ha, 0.47f + 0.06f * ha, 1.0f, alpha));   // accent blue
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{buttons[1], 14, 14}, brush_.Get());
    IDWriteTextFormat* buttonFont = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    DrawText(dc, L"Reject", buttonFont, buttons[0], WithAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(dc, L"Approve", buttonFont, buttons[1], D2D1::ColorF(1, 1, 1, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
    const std::wstring deferLabel = L"Answer in " + r.source;
    DrawText(dc, deferLabel.c_str(), smallFont, buttons[2], WithAlpha(kTextDim, (1.0f + 0.6f * hd) * alpha));
}

// ---- messages --------------------------------------------------------------------------------

float IslandWindow::TargetHeight() const {
    if (popover_ >= 0) return std::clamp(MenuHeight(popover_), kExpandedH, kMaxPanelH);
    return kExpandedH;
}

void IslandWindow::ApplyHeightTarget() {
    if (!expanded_) return;
    const float target = TargetHeight();
    if (target != h_.target) {
        SetOpenSprings(w_, h_);
        h_.target = target;
        SetHitRegion(true);                       // cover the new height before the panel gets there
        Animate();
    }
}

// ---- system ----------------------------------------------------------------------------------

D2D1_RECT_F IslandWindow::RingCell(const D2D1_RECT_F& panel, int index) const {
    const float x0 = panel.left + 24;
    const float x1 = panel.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    const float w = (x1 - x0) / kMetricCount;
    return D2D1::RectF(x0 + w * index, panel.top + 54, x0 + w * (index + 1), panel.bottom - 8);
}

void IslandWindow::DrawRing(ID2D1DeviceContext* dc, D2D1_POINT_2F c, float r, float thickness, float fraction,
                            D2D1_COLOR_F color, float alpha, float trackAlpha) {
    if (!roundStroke_) {
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        gfx_.d2dFactory()->CreateStrokeStyle(props, nullptr, 0, &roundStroke_);
    }
    brush_->SetColor(D2D1::ColorF(1, 1, 1, trackAlpha * alpha));
    dc->DrawEllipse(D2D1::Ellipse(c, r, r), brush_.Get(), thickness);

    fraction = Clamp01(fraction);
    if (fraction <= 0.004f) return;
    auto stroke = [&](float width, float a) {
        brush_->SetColor(D2D1::ColorF(color.r, color.g, color.b, a * alpha));
        if (fraction >= 0.996f) {
            dc->DrawEllipse(D2D1::Ellipse(c, r, r), brush_.Get(), width);
            return;
        }
        Com<ID2D1PathGeometry> geometry;
        Com<ID2D1GeometrySink> sink;
        if (FAILED(gfx_.d2dFactory()->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return;
        const float end = -1.5707963f + fraction * 6.2831853f;           // clockwise from 12 o'clock
        sink->BeginFigure(D2D1::Point2F(c.x, c.y - r), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + r * std::cos(end), c.y + r * std::sin(end)), D2D1::SizeF(r, r), 0,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      fraction > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        dc->DrawGeometry(geometry.Get(), brush_.Get(), width, roundStroke_.Get());
    };
    stroke(thickness + 6.0f, 0.13f);                                     // soft glow
    stroke(thickness, 1.0f);
}

void IslandWindow::DrawSystem(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    const auto metrics = BuildMetrics(system_);
    IDWriteTextFormat* bigFont = gfx_.Text(17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* unitFont = gfx_.Text(9.5f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* labelFont = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* captionFont = gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL);

    for (int i = 0; i < kMetricCount; ++i) {
        const Metric& metric = metrics[i];
        const SystemPage& page = metric.pages[ringShown_[i] % metric.pages.size()];
        const D2D1_RECT_F cell = RingCell(base, i);
        const float hover = ringHover_[i], press = ringPress_[i];
        const float scale = 1.0f + 0.07f * hover - 0.06f * press;
        const D2D1_POINT_2F c{(cell.left + cell.right) / 2, cell.top + kRingRadius + 4};
        const D2D1_COLOR_F color = page.severity >= 2 ? kRed : page.severity == 1 ? kAmber : kAccent;

        // A faint disc lifts the ring under the pointer.
        if (hover > 0.01f) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.05f * hover * alpha));
            dc->FillEllipse(D2D1::Ellipse(c, (kRingRadius + 7) * scale, (kRingRadius + 7) * scale), brush_.Get());
        }
        DrawRing(dc, c, kRingRadius * scale, kRingThickness * scale, ringFrac_[i], color, alpha, 0.085f + 0.04f * hover);

        // Centre: value (and its unit under it), cross-faded when the page changes.
        const float a = alpha * ringFade_[i];
        const D2D1_COLOR_F valueColor = page.severity >= 2 ? kRed : page.severity == 1 ? kAmber : kText;
        const float box = kRingRadius * 1.5f;
        if (page.unit.empty()) {
            DrawText(dc, page.value.c_str(), bigFont, D2D1::RectF(c.x - box, c.y - 12, c.x + box, c.y + 12),
                     WithAlpha(valueColor, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            DrawText(dc, page.value.c_str(), bigFont, D2D1::RectF(c.x - box, c.y - 15, c.x + box, c.y + 5),
                     WithAlpha(valueColor, a), DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawText(dc, page.unit.c_str(), unitFont, D2D1::RectF(c.x - box, c.y + 3, c.x + box, c.y + 15),
                     WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Page dots, then the label and a caption for the page shown.
        const int pages = static_cast<int>(metric.pages.size());
        const float dotsY = cell.top + 2 * kRingRadius + 12;
        if (pages > 1) {
            const float startX = c.x - (pages - 1) * 4.0f;
            for (int d = 0; d < pages; ++d) {
                const bool active = d == ringPage_[i] % pages;
                brush_->SetColor(D2D1::ColorF(1, 1, 1, (active ? 0.75f : 0.22f) * alpha));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(startX + d * 8.0f, dotsY), active ? 1.8f : 1.4f, active ? 1.8f : 1.4f),
                                brush_.Get());
            }
        }
        DrawText(dc, metric.label, labelFont, D2D1::RectF(cell.left, dotsY + 5, cell.right, dotsY + 19),
                 WithAlpha(kText, (0.82f + 0.16f * hover) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, page.caption.c_str(), captionFont, D2D1::RectF(cell.left - 2, dotsY + 19, cell.right + 2, dotsY + 33),
                 WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

// ---- audio: buttons and menu -------------------------------------------------------------------

// ---- audio, camera and Solu: round buttons and the menu inside the island ----------------------------

IslandWindow::Dot IslandWindow::SideDot(int side) const {
    if (side == 3) {                                              // Solu: grey = off, amber = busy, green = awake
        switch (solu_.state) {
        case SoluState::Off:
        case SoluState::Missing: return Dot::Unavailable;
        case SoluState::Loading:
        case SoluState::Thinking: return Dot::InUse;
        default: return Dot::Ready;
        }
    }
    if (!SideValid(side)) return Dot::Unavailable;
    if (side == 2) {
        bool anyReady = false;
        for (const auto& d : camera_.devices) anyReady |= d.ready;
        if (!anyReady) return Dot::Unavailable;
        return camera_.inUseBy.empty() ? Dot::Ready : Dot::InUse;
    }
    const AudioSide& s = VoiceSide(side);
    if (!s.connected || !s.ready) return Dot::Unavailable;
    return side == 0 && !s.inUseBy.empty() ? Dot::InUse : Dot::Ready;     // a playing speaker is just "ready"
}

std::wstring IslandWindow::SideInUse(int side) const {
    if (side == 3) return {};
    const std::vector<std::wstring>& apps = side == 2 ? camera_.inUseBy : VoiceSide(side).inUseBy;
    std::wstring text;
    for (size_t i = 0; i < apps.size() && i < 2; ++i) text += (i ? L", " : L"") + apps[i];
    if (apps.size() > 2) text += L" +" + std::to_wstring(apps.size() - 2);
    return text;
}

std::wstring IslandWindow::SideHint(int side) const {
    if (side == 3) return solu_.error;
    if (!SideValid(side)) return {};
    if (side == 2) {
        if (camera_.devices.empty()) return L"No camera found";
        for (const auto& d : camera_.devices)
            if (!d.ready) return d.hint;
        return {};
    }
    const AudioSide& s = VoiceSide(side);
    return s.connected ? s.hint : L"Not connected";
}

bool IslandWindow::SoluActive() const {
    return solu_.state == SoluState::Listening || solu_.state == SoluState::Thinking || solu_.state == SoluState::Speaking;
}

void IslandWindow::DrawAudioButtons(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    if (!roundStroke_) {
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        gfx_.d2dFactory()->CreateStrokeStyle(props, nullptr, 0, &roundStroke_);
    }
    IDWriteTextFormat* glyphFont = gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_NORMAL, true);
    const wchar_t* const glyphs[4] = {L"", L"", L"", L""};     // microphone, speaker, camera, gear
    const float now = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;
    for (int side = 0; side < 4; ++side) {
        const Dot dot = SideDot(side);
        const bool muted = side < 3 && SideMuted(side);
        const bool usable = dot != Dot::Unavailable;
        const D2D1_RECT_F r = AudioButton(base, side);
        const D2D1_POINT_2F c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        const float hover = audioHover_[side], open = audioOpen_[side];

        brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.075f + 0.07f * hover + 0.10f * open) * alpha));
        dc->FillEllipse(D2D1::Ellipse(c, kAudioBtn / 2, kAudioBtn / 2), brush_.Get());

        // A soft green ring breathes with the live level (what Solu hears / what is playing); an amber ring
        // means an app is using the camera right now.
        const float level = side < 2 && usable && !muted ? Clamp01(std::sqrt(voiceLevel_[side]) * 1.2f) : 0.0f;
        if (level > 0.05f) {
            brush_->SetColor(D2D1::ColorF(kGreen.r, kGreen.g, kGreen.b, std::min(0.75f, level) * alpha));
            dc->DrawEllipse(D2D1::Ellipse(c, kAudioBtn / 2 + 1.2f + 1.6f * level, kAudioBtn / 2 + 1.2f + 1.6f * level), brush_.Get(), 1.3f);
        }
        if (side == 2 && dot == Dot::InUse && !muted) {
            brush_->SetColor(D2D1::ColorF(kAmber.r, kAmber.g, kAmber.b, 0.85f * alpha));
            dc->DrawEllipse(D2D1::Ellipse(c, kAudioBtn / 2 + 2.0f, kAudioBtn / 2 + 2.0f), brush_.Get(), 1.4f);
        }
        DrawText(dc, glyphs[side], glyphFont, r,
                 WithAlpha(muted ? kRed : kText, ((usable || muted || side == 3) ? 0.92f : 0.40f + 0.25f * hover) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (muted) {                                                // a slash through it, with a thin gap like SF Symbols
            const D2D1_POINT_2F p0{c.x - 7.0f, c.y + 7.0f}, p1{c.x + 7.0f, c.y - 7.0f};
            brush_->SetColor(D2D1::ColorF(0.118f, 0.118f, 0.130f, alpha));
            dc->DrawLine(D2D1::Point2F(p0.x - 1.2f, p0.y + 0.2f), D2D1::Point2F(p1.x - 1.2f, p1.y + 0.2f), brush_.Get(), 3.4f, roundStroke_.Get());
            brush_->SetColor(WithAlpha(kRed, alpha));
            dc->DrawLine(p0, p1, brush_.Get(), 1.8f, roundStroke_.Get());
        }

        // Status light, cut into the button with a ring in the island's colour:
        // green = connected and ready, amber = in use right now, grey = not available (connect it first).
        if (side != 3) {                                             // (the gear is just settings: no status light)
            const D2D1_POINT_2F d{c.x + 10.5f, c.y + 10.5f};
            const D2D1_COLOR_F light = dot == Dot::Ready ? kGreen : dot == Dot::InUse ? kAmber : kNeutral;
            brush_->SetColor(WithAlpha(kIslandFill, alpha));
            dc->FillEllipse(D2D1::Ellipse(d, 5.4f, 5.4f), brush_.Get());
            brush_->SetColor(WithAlpha(light, alpha));
            dc->FillEllipse(D2D1::Ellipse(d, 3.4f, 3.4f), brush_.Get());
        }
    }
}

D2D1_RECT_F IslandWindow::TestPill(const PopoverLayout& g, int side) const {
    const float titleW = std::ceil(gfx_.MeasureWidth(side == 0 ? L"Microphone" : L"Speaker", gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD)));
    const float left = g.card.left + 30 + titleW + 10;
    return D2D1::RectF(left, g.card.top + 3, left + 46, g.card.top + 23);
}

void IslandWindow::DrawAudioPopover(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    const float t = popoverT_;
    if (t < 0.003f || popoverSide_ < 0) return;
    if (!roundStroke_) {
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        gfx_.d2dFactory()->CreateStrokeStyle(props, nullptr, 0, &roundStroke_);
    }
    const int side = popoverSide_;
    const AudioSide* s = side < 2 ? &VoiceSide(side) : nullptr;     // null for the camera and Solu
    const float a = alpha * Clamp01(t * 1.15f - 0.15f);            // the old tab content fades out first, then this fades in
    const float slide = (1.0f - t) * 8.0f;                          // and rises into place while the island grows
    const PopoverLayout g = PopoverGeometry(D2D1::RectF(base.left, base.top + slide, base.right, base.bottom + slide));
    const D2D1_RECT_F area = g.card;
    IDWriteTextFormat* quiet = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* mainFont = gfx_.Text(13.5f, DWRITE_FONT_WEIGHT_NORMAL);
    if (side == 3) {                                                // the gear: general settings
        DrawSettingsMenu(dc, g, a);
        return;
    }

    // ---- what to say about this device ------------------------------------------------------------------------------
    const bool valid = SideValid(side), muted = SideMuted(side);
    const Dot dot = SideDot(side);
    std::wstring statusText;
    D2D1_COLOR_F light = kNeutral;
    if (side == 3) {
        switch (solu_.state) {
        case SoluState::Off: statusText = solu_.error.empty() ? L"Off" : solu_.error; light = solu_.error.empty() ? kNeutral : kRed; break;
        case SoluState::Missing: statusText = L"Not installed"; light = kRed; break;
        case SoluState::Loading: statusText = L"Waking up…"; light = kAmber; break;
        case SoluState::Ready: statusText = L"Ready · Ctrl+Alt+Space"; light = kGreen; break;
        case SoluState::Listening: statusText = L"Listening"; light = kGreen; break;
        case SoluState::Thinking: statusText = L"Thinking…"; light = kAmber; break;
        case SoluState::Speaking: statusText = L"Speaking"; light = kGreen; break;
        }
        if (!solu_.error.empty() && solu_.state != SoluState::Off && solu_.state != SoluState::Missing) { statusText = solu_.error; light = kRed; }
    } else if (!valid) {
        statusText = L"Looking…";
    } else if (muted) {
        statusText = side == 2 ? L"Blocked" : L"Muted";
        light = kRed;
    } else if (dot == Dot::Unavailable) {
        statusText = SideHint(side);
        if (statusText.empty()) statusText = L"Not connected";
    } else if (dot == Dot::InUse) {
        statusText = L"In use · " + SideInUse(side);
        light = kAmber;
    } else {
        statusText = L"Ready";
        light = kGreen;
    }
    // A finished test replaces the status for a few seconds.
    const AudioTest& test = audio_.test;
    const bool mine = side < 2 && test.input == (side == 0);
    const bool testing = mine && test.state == AudioTest::State::Running;
    if (mine && test.state == AudioTest::State::Done && GetTickCount64() - test.doneAt < 6000) {
        switch (test.verdict) {
        case AudioTest::Verdict::Works: statusText = side == 0 ? L"Microphone works" : L"Test sound played"; light = kGreen; break;
        case AudioTest::Verdict::Quiet: statusText = L"Very quiet — speak closer"; light = kAmber; break;
        case AudioTest::Verdict::Silent: statusText = dot == Dot::Unavailable && !SideHint(side).empty() ? SideHint(side) : L"No sound heard"; light = kRed; break;
        case AudioTest::Verdict::Blocked: statusText = L"Blocked in Windows privacy"; light = kRed; break;
        case AudioTest::Verdict::Muted: statusText = L"Muted — switch it on first"; light = kRed; break;
        default: statusText = L"Couldn't run the test"; light = kRed; break;
        }
    }

    // ---- header: glyph, title and Test on the left; state (or the live meter) and the on/off switch on the right -------
    const float headTop = area.top;
    const wchar_t* const glyphs[4] = {L"", L"", L"", L""};
    const wchar_t* const titles[4] = {L"Microphone", L"Speaker", L"Camera", L"Solu"};
    DrawText(dc, glyphs[side], gfx_.Text(15.0f, DWRITE_FONT_WEIGHT_NORMAL, true),
             D2D1::RectF(area.left, headTop, area.left + 24, headTop + 26), WithAlpha(kText, 0.92f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(dc, titles[side], gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             D2D1::RectF(area.left + 30, headTop, area.left + 130, headTop + 26), WithAlpha(kText, a));
    float slotLeft = area.left + 30 + std::ceil(gfx_.MeasureWidth(titles[side], gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD))) + 12.0f;
    if (side < 2) {                                                 // the compact Test control
        const D2D1_RECT_F pill = TestPill(g, side);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.10f + 0.10f * testHover_ + (testing ? 0.08f : 0.0f)) * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{pill, 10, 10}, brush_.Get());
        DrawText(dc, testing ? L"Stop" : L"Test", gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), pill,
                 WithAlpha(kText, (0.82f + 0.15f * testHover_) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        slotLeft = pill.right + 14.0f;
    }

    const D2D1_RECT_F sw = MuteSwitch(g);                          // Apple-style switch: on = sound / camera / Solu on
    const float on = muteAnim_[side];
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.20f * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{sw, 12, 12}, brush_.Get());
    brush_->SetColor(D2D1::ColorF(0.20f, 0.78f, 0.35f, on * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{sw, 12, 12}, brush_.Get());
    const D2D1_POINT_2F knob{sw.left + 12.0f + on * 16.0f, (sw.top + sw.bottom) / 2};
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.28f * a));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob.x, knob.y + 1.2f), 10.0f, 10.0f), brush_.Get());
    brush_->SetColor(D2D1::ColorF(1, 1, 1, a));
    dc->FillEllipse(D2D1::Ellipse(knob, 10.0f, 10.0f), brush_.Get());

    const float slotRight = sw.left - 14.0f;
    if (testing) {                                                  // a scrolling waveform of what is being heard / played
        const wchar_t* label = side == 0 ? L"Listening" : L"Playing";
        const float labelW = std::ceil(gfx_.MeasureWidth(label, quiet));
        DrawText(dc, label, quiet, D2D1::RectF(slotLeft, headTop + 5, slotLeft + labelW + 4, headTop + 21), WithAlpha(kTextDim, 1.2f * a));
        const float barsLeft = slotLeft + labelW + 10.0f;
        const int bars = std::clamp(static_cast<int>((slotRight - barsLeft) / 4.4f), 4, static_cast<int>(testBars_.size()));
        for (int i = 0; i < bars; ++i) {
            const float v = testBars_[testBars_.size() - static_cast<size_t>(bars) + static_cast<size_t>(i)];
            const float h = 3.0f + 15.0f * v;
            const float x = barsLeft + i * 4.4f;
            brush_->SetColor(D2D1::ColorF(kGreen.r, kGreen.g, kGreen.b, (0.25f + 0.70f * (static_cast<float>(i + 1) / bars)) * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, headTop + 13 - h / 2, x + 2.6f, headTop + 13 + h / 2), 1.3f, 1.3f}, brush_.Get());
        }
    } else {
        const float textRight = slotRight;
        const float textW = std::min(textRight - slotLeft - 12.0f, std::ceil(gfx_.MeasureWidth(statusText.c_str(), quiet)));
        brush_->SetColor(D2D1::ColorF(light.r, light.g, light.b, a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(textRight - textW - 9, headTop + 13), 3.0f, 3.0f), brush_.Get());
        DrawText(dc, statusText.c_str(), quiet, D2D1::RectF(textRight - textW - 3, headTop + 5, textRight + 3, headTop + 21),
                 WithAlpha(kTextDim, 1.15f * a), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * a));
    dc->DrawLine(D2D1::Point2F(area.left, g.rowsTop - 1.5f), D2D1::Point2F(area.right, g.rowsTop - 1.5f), brush_.Get(), 1.0f);

    // ---- rows: microphones/speakers, Solu's voices, or the cameras ---------------------------------------------------
    const float textLeft = area.left + 38, textRight = area.right - (side == 3 ? 12.0f : 24.0f);
    dc->PushAxisAlignedClip(D2D1::RectF(area.left - 10, g.rowsTop - 1, area.right + 10, g.footer.top), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < g.shown; ++i) {
        const int k = voiceScroll_ + i;
        if (k >= g.total) break;
        const D2D1_RECT_F row = D2D1::RectF(area.left, g.rowsTop + i * kPopRowH, area.right, g.rowsTop + (i + 1) * kPopRowH);
        const float hover = side != 2 ? popRowHover_[static_cast<size_t>(i)] : 0.0f;
        if (hover > 0.01f) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.075f * hover * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(row.left - 8, row.top + 2, row.right + 6, row.bottom - 2), 11, 11}, brush_.Get());
        }
        std::wstring main, sub;
        bool selected = false, ready = true;
        if (side == 3) {                                             // Solu's voices
            main = kSoluVoices[k].name;
            sub = kSoluVoices[k].description;
            selected = solu_.voice == kSoluVoices[k].id;
        } else if (side == 2) {                                      // cameras
            if (camera_.devices.empty()) {
                main = L"No camera found";
                sub = L"Plug one in, or connect your phone";
                ready = false;
            } else {
                const CameraDevice& cam = camera_.devices[static_cast<size_t>(k)];
                SplitDeviceName(cam.name, main, sub);
                ready = cam.ready;
                if (!cam.ready) sub = cam.hint;
                else if (sub.empty()) sub = L"Camera";
            }
        } else if (s->devices.empty()) {
            main = L"No device found";
            sub = L"Plug one in";
            ready = false;
        } else {
            const AudioDevice& dev = s->devices[static_cast<size_t>(k)];
            SplitDeviceName(dev.name, main, sub);
            selected = dev.isDefault;                                // the one Windows is using
            ready = dev.ready;
            if (!dev.ready) sub = dev.hint;
        }
        const float dim = ready ? 1.0f : 0.55f;
        DrawText(dc, main.c_str(), mainFont, D2D1::RectF(textLeft, row.top + 5, textRight, row.top + 23), WithAlpha(kText, 0.97f * dim * a));
        DrawText(dc, sub.c_str(), quiet, D2D1::RectF(textLeft, row.top + 22, textRight, row.top + 37), WithAlpha(kTextDim, 1.1f * a));
        if (side != 3) {                                             // per-device light: green ready, grey not available
            const bool inUse = side == 2 && ready && !camera_.inUseBy.empty();
            const D2D1_COLOR_F dc2 = !ready ? kNeutral : inUse ? kAmber : kGreen;
            brush_->SetColor(D2D1::ColorF(dc2.r, dc2.g, dc2.b, (ready ? 0.95f : 0.7f) * a));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(area.right - 10, (row.top + row.bottom) / 2), 3.0f, 3.0f), brush_.Get());
        }
        if (selected) {                                              // the checkmark, drawn as a stroke so it stays crisp
            const float x = area.left + 16, y = (row.top + row.bottom) / 2;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.96f * a));
            dc->DrawLine(D2D1::Point2F(x - 5.0f, y + 0.5f), D2D1::Point2F(x - 1.6f, y + 4.0f), brush_.Get(), 2.0f, roundStroke_.Get());
            dc->DrawLine(D2D1::Point2F(x - 1.6f, y + 4.0f), D2D1::Point2F(x + 5.2f, y - 4.2f), brush_.Get(), 2.0f, roundStroke_.Get());
        }
        if (i < g.shown - 1 && k < g.total - 1) {                    // hairline between rows, starting at the text
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.085f * a));
            dc->DrawLine(D2D1::Point2F(textLeft, row.bottom - 0.5f), D2D1::Point2F(area.right, row.bottom - 0.5f), brush_.Get(), 1.0f);
        }
    }
    dc->PopAxisAlignedClip();
    if (g.total > g.shown) {                                         // thin scrollbar for long lists
        const float trackH = g.shown * kPopRowH - 12.0f;
        const float thumbH = std::max(18.0f, trackH * g.shown / g.total);
        const float thumbTop = g.rowsTop + 6.0f + (trackH - thumbH) * (voiceScroll_ / static_cast<float>(g.total - g.shown));
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.24f * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(area.right + 3, thumbTop, area.right + 5.5f, thumbTop + thumbH), 1.3f, 1.3f},
                                 brush_.Get());
    }

    // ---- footer ---------------------------------------------------------------------------------------
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * a));
    dc->DrawLine(D2D1::Point2F(area.left, g.footer.top - 0.5f), D2D1::Point2F(area.right, g.footer.top - 0.5f), brush_.Get(), 1.0f);
    if (popFooterHover_ > 0.01f) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.075f * popFooterHover_ * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(g.footer.left - 8, g.footer.top + 2, g.footer.right + 6, g.footer.bottom - 2), 11, 11},
                                 brush_.Get());
    }
    if (side == 3) {                                                 // "I'm wearing headphones": lets you talk over Solu
        DrawText(dc, L"Headphones mode", mainFont, D2D1::RectF(g.footer.left + 4, g.footer.top, g.footer.right - 60, g.footer.bottom),
                 WithAlpha(kText, (0.86f + 0.1f * popFooterHover_) * a));
        DrawText(dc, solu_.headphones ? L"talk over Solu" : L"Solu waits for you", quiet,
                 D2D1::RectF(g.footer.right - 175, g.footer.top, g.footer.right - 50, g.footer.bottom), WithAlpha(kTextDim, 1.1f * a),
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        const D2D1_RECT_F mini = D2D1::RectF(g.footer.right - 40, g.footer.top + (kPopFooterH - 20) / 2, g.footer.right - 4, g.footer.top + (kPopFooterH + 20) / 2);
        const float hp = solu_.headphones ? 1.0f : 0.0f;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.20f * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{mini, 10, 10}, brush_.Get());
        brush_->SetColor(D2D1::ColorF(0.20f, 0.78f, 0.35f, hp * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{mini, 10, 10}, brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mini.left + 10 + hp * 16.0f, (mini.top + mini.bottom) / 2), 8.0f, 8.0f), brush_.Get());
    } else {
        DrawText(dc, side == 2 ? L"Camera privacy settings…" : L"Sound settings…", mainFont, g.footer,
                 WithAlpha(kText, (0.86f + 0.1f * popFooterHover_) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

D2D1_RECT_F IslandWindow::SoluCaptionRect(const D2D1_RECT_F& base) const {
    return D2D1::RectF(base.left + 22, base.top + kTabTop + kTabH + 8.0f, base.left + kExpandedW - (mascot_ ? kMascotLane : 22.0f),
                       base.top + kExpandedH - 14.0f);
}

// The live conversation: what Solu is doing, what you said and what it answered.
void IslandWindow::DrawSoluCaption(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    const float t = captionT_;
    if (t < 0.003f) return;
    const float a = alpha * Clamp01(t * 1.2f - 0.2f);
    const float slide = (1.0f - t) * 8.0f;
    const D2D1_RECT_F r = SoluCaptionRect(D2D1::RectF(base.left, base.top + slide, base.right, base.bottom + slide));
    IDWriteTextFormat* quiet = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* heardFont = gfx_.Wrapped(13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* saidFont = gfx_.Wrapped(15.0f, DWRITE_FONT_WEIGHT_NORMAL);
    const float now = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;

    const wchar_t* label = solu_.state == SoluState::Listening ? L"LISTENING" : solu_.state == SoluState::Thinking ? L"THINKING"
                         : solu_.state == SoluState::Speaking ? L"SOLU" : L"";
    const D2D1_COLOR_F tint = solu_.state == SoluState::Thinking ? kAmber : kGreen;
    brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, a));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 4, r.top + 8), 3.0f, 3.0f), brush_.Get());
    DrawText(dc, label, quiet, D2D1::RectF(r.left + 14, r.top, r.left + 120, r.top + 16), WithAlpha(kTextDim, 1.25f * a));

    // On the right of the label: the microphone while listening, pulsing dots while thinking.
    const float right = r.right;
    if (solu_.state == SoluState::Listening) {
        for (int i = 0; i < 18; ++i) {
            const float v = soluBars_[soluBars_.size() - 18 + static_cast<size_t>(i)];
            const float h = 3.0f + 12.0f * v;
            const float x = right - 18 * 4.4f + i * 4.4f;
            brush_->SetColor(D2D1::ColorF(kGreen.r, kGreen.g, kGreen.b, (0.25f + 0.7f * (i + 1) / 18.0f) * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, r.top + 8 - h / 2, x + 2.6f, r.top + 8 + h / 2), 1.3f, 1.3f}, brush_.Get());
        }
    } else if (solu_.state == SoluState::Thinking) {
        for (int i = 0; i < 3; ++i) {
            const float p = 0.5f + 0.5f * std::sin(now * 6.0f - i * 0.9f);
            brush_->SetColor(D2D1::ColorF(kAmber.r, kAmber.g, kAmber.b, (0.3f + 0.6f * p) * a));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(right - 24 + i * 10.0f, r.top + 8), 2.8f, 2.8f), brush_.Get());
        }
    }

    float y = r.top + 24;
    if (!solu_.heard.empty()) {                                      // what you said
        DrawText(dc, solu_.heard.c_str(), heardFont, D2D1::RectF(r.left, y, r.right, y + 38), WithAlpha(kTextDim, 1.3f * a),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        y += 40;
    } else if (solu_.state == SoluState::Listening) {
        DrawText(dc, L"Say something…", gfx_.Text(15.0f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left, y + 10, r.right, y + 34),
                 WithAlpha(kTextDim, 1.1f * a));
    }
    if (!solu_.said.empty())                                         // what Solu answered
        DrawText(dc, solu_.said.c_str(), saidFont, D2D1::RectF(r.left, y, r.right, r.bottom), WithAlpha(kText, 0.97f * a),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

// ---- the gear: general settings (Solu's options for now, room for more) ---------------------------------

void IslandWindow::DrawSettingsMenu(ID2D1DeviceContext* dc, const PopoverLayout& g, float a) {
    const D2D1_RECT_F area = g.card;
    IDWriteTextFormat* quiet = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* mainFont = gfx_.Text(13.5f, DWRITE_FONT_WEIGHT_NORMAL);
    const bool voicePage = settingsPage_ == 1;
    const float headTop = area.top;

    // ---- header ----------------------------------------------------------------------------------------------
    if (voicePage) {                                                // "< Voice": tap to go back
        const float hot = hover_ == kHitPopBack ? 1.0f : 0.0f;
        if (hot > 0) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.075f * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(area.left - 6, headTop - 2, area.left + 104, headTop + 26), 11, 11}, brush_.Get());
        }
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.78f + 0.2f * hot) * a));
        dc->DrawLine(D2D1::Point2F(area.left + 10, headTop + 7), D2D1::Point2F(area.left + 4, headTop + 13), brush_.Get(), 1.9f, roundStroke_.Get());
        dc->DrawLine(D2D1::Point2F(area.left + 4, headTop + 13), D2D1::Point2F(area.left + 10, headTop + 19), brush_.Get(), 1.9f, roundStroke_.Get());
        DrawText(dc, L"Voice", gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(area.left + 22, headTop, area.left + 130, headTop + 26),
                 WithAlpha(kText, a));
    } else {
        DrawText(dc, L"", gfx_.Text(15.0f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(area.left, headTop, area.left + 24, headTop + 26),
                 WithAlpha(kText, 0.92f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"Settings", gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(area.left + 30, headTop, area.left + 160, headTop + 26),
                 WithAlpha(kText, a));
    }
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * a));
    dc->DrawLine(D2D1::Point2F(area.left, g.rowsTop - 1.5f), D2D1::Point2F(area.right, g.rowsTop - 1.5f), brush_.Get(), 1.0f);

    // ---- rows ------------------------------------------------------------------------------------------------
    std::wstring voiceName = L"Cozy", voiceNote;
    for (const SoluVoice& v : kSoluVoices)
        if (solu_.voice == v.id) voiceName = v.name, voiceNote = v.description;
    std::wstring soluNote;
    switch (solu_.state) {
    case SoluState::Off: soluNote = solu_.error.empty() ? L"Off — switch on to talk hands-free" : solu_.error; break;
    case SoluState::Missing: soluNote = L"Not installed (run tools\\setup_solu.ps1)"; break;
    case SoluState::Loading: soluNote = L"Waking up…"; break;
    case SoluState::Ready: soluNote = L"Listening for “Solu”"; break;
    case SoluState::Listening: soluNote = L"In a conversation"; break;
    case SoluState::Thinking: soluNote = L"Thinking…"; break;
    case SoluState::Speaking: soluNote = L"Speaking"; break;
    }
    const float textLeft = voicePage ? area.left + 38 : area.left + 8, textRight = area.right - 60;
    dc->PushAxisAlignedClip(D2D1::RectF(area.left - 10, g.rowsTop - 1, area.right + 10, g.footer.top), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < g.shown; ++i) {
        const int k = voiceScroll_ + i;
        if (k >= g.total) break;
        const D2D1_RECT_F row = D2D1::RectF(area.left, g.rowsTop + i * kPopRowH, area.right, g.rowsTop + (i + 1) * kPopRowH);
        const float hover = popRowHover_[static_cast<size_t>(i)];
        if (hover > 0.01f) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.075f * hover * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(row.left - 8, row.top + 2, row.right + 6, row.bottom - 2), 11, 11}, brush_.Get());
        }
        std::wstring title, note;
        bool isSwitch = false, on = false, chevron = false, selected = false;
        if (!voicePage) {
            switch (k) {
            case 0: title = L"Solu AI"; note = soluNote; isSwitch = true; on = solu_.enabled; break;
            case 1: title = L"Live captions"; note = L"Show the conversation on the island"; isSwitch = true; on = solu_.captions; break;
            case 2: title = L"Headphones mode"; note = solu_.headphones ? L"You can talk over Solu" : L"Solu waits until it has finished talking";
                    isSwitch = true; on = solu_.headphones; break;
            default: title = L"Voice"; note = voiceName + L" · " + voiceNote; chevron = true; break;
            }
        } else {
            title = kSoluVoices[k].name;
            note = kSoluVoices[k].description;
            selected = solu_.voice == kSoluVoices[k].id;
        }
        const float tl = voicePage ? textLeft : area.left + 8;
        DrawText(dc, title.c_str(), mainFont, D2D1::RectF(tl, row.top + 5, textRight, row.top + 23), WithAlpha(kText, 0.97f * a));
        DrawText(dc, note.c_str(), quiet, D2D1::RectF(tl, row.top + 22, isSwitch || chevron ? textRight : area.right - 12, row.top + 37),
                 WithAlpha(kTextDim, 1.1f * a));
        const float cy = (row.top + row.bottom) / 2;
        if (isSwitch) {                                              // the same Apple-style switch, small
            const D2D1_RECT_F sw = D2D1::RectF(area.right - 48, cy - 11, area.right - 8, cy + 11);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.20f * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{sw, 11, 11}, brush_.Get());
            if (on) {
                brush_->SetColor(D2D1::ColorF(0.20f, 0.78f, 0.35f, a));
                dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{sw, 11, 11}, brush_.Get());
            }
            const D2D1_POINT_2F knob{on ? sw.right - 11.0f : sw.left + 11.0f, cy};
            brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.28f * a));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob.x, knob.y + 1.2f), 9.0f, 9.0f), brush_.Get());
            brush_->SetColor(D2D1::ColorF(1, 1, 1, a));
            dc->FillEllipse(D2D1::Ellipse(knob, 9.0f, 9.0f), brush_.Get());
        }
        if (chevron) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.40f + 0.4f * hover) * a));
            dc->DrawLine(D2D1::Point2F(area.right - 20, cy - 4), D2D1::Point2F(area.right - 16, cy), brush_.Get(), 1.7f, roundStroke_.Get());
            dc->DrawLine(D2D1::Point2F(area.right - 16, cy), D2D1::Point2F(area.right - 20, cy + 4), brush_.Get(), 1.7f, roundStroke_.Get());
        }
        if (selected) {
            const float x = area.left + 16;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.96f * a));
            dc->DrawLine(D2D1::Point2F(x - 5.0f, cy + 0.5f), D2D1::Point2F(x - 1.6f, cy + 4.0f), brush_.Get(), 2.0f, roundStroke_.Get());
            dc->DrawLine(D2D1::Point2F(x - 1.6f, cy + 4.0f), D2D1::Point2F(x + 5.2f, cy - 4.2f), brush_.Get(), 2.0f, roundStroke_.Get());
        }
        if (i < g.shown - 1 && k < g.total - 1) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.085f * a));
            dc->DrawLine(D2D1::Point2F(tl, row.bottom - 0.5f), D2D1::Point2F(area.right, row.bottom - 0.5f), brush_.Get(), 1.0f);
        }
    }
    dc->PopAxisAlignedClip();

    // ---- footer: only a hint -----------------------------------------------------------------------------------------
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * a));
    dc->DrawLine(D2D1::Point2F(area.left, g.footer.top - 0.5f), D2D1::Point2F(area.right, g.footer.top - 0.5f), brush_.Get(), 1.0f);
    DrawText(dc, voicePage ? L"Solu says hello in the voice you pick" : L"Say “Solu” to talk · or press Ctrl+Alt+Space", quiet, g.footer,
             WithAlpha(kTextDim, 1.15f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---- the Solu AI tab -------------------------------------------------------------------------------------

void IslandWindow::DrawSolu(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    const float left = base.left + 26, right = base.left + kExpandedW - (mascot_ ? kMascotLane : 26.0f);
    float y = base.top + kTabTop + kTabH + 14.0f;
    std::wstring title, sub;
    D2D1_COLOR_F tint = kNeutral;
    switch (solu_.state) {
    case SoluState::Off:
        title = L"Solu is off";
        sub = solu_.error.empty() ? L"Switch it on in Settings (the gear) and just say “Solu”." : solu_.error;
        break;
    case SoluState::Missing: title = L"Solu isn't installed"; sub = L"Run tools\\setup_solu.ps1 once, then switch it on."; tint = kRed; break;
    case SoluState::Loading: title = L"Solu is waking up…"; sub = L"It loads its models, a few seconds."; tint = kAmber; break;
    case SoluState::Ready: title = L"Solu is listening"; sub = L"Say “Solu”, or press Ctrl+Alt+Space."; tint = kGreen; break;
    case SoluState::Listening: title = L"I'm listening"; sub = L"Go ahead."; tint = kGreen; break;
    case SoluState::Thinking: title = L"Thinking…"; sub = L""; tint = kAmber; break;
    case SoluState::Speaking: title = L"Solu"; sub = L""; tint = kGreen; break;
    }
    brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, alpha));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 4, y + 11), 4.0f, 4.0f), brush_.Get());
    DrawText(dc, title.c_str(), gfx_.Text(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left + 18, y, right, y + 22), WithAlpha(kText, alpha));
    DrawText(dc, sub.c_str(), gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(left + 18, y + 22, right, y + 40), WithAlpha(kTextDim, alpha));
    y += 52.0f;

    if (!solu_.history.empty()) {                                    // the last exchange
        const SoluExchange& ex = solu_.history.back();
        DrawText(dc, ex.heard.c_str(), gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(left, y, right, y + 18), WithAlpha(kTextDim, 1.2f * alpha));
        DrawText(dc, ex.said.c_str(), gfx_.Wrapped(13.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(left, y + 20, right, base.top + kExpandedH - 12),
                 WithAlpha(kText, 0.95f * alpha), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        return;
    }
    // Nothing said yet: show what can be said.
    IDWriteTextFormat* chipFont = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    const wchar_t* const phrases[] = {L"“Solu, what's up?”", L"“Solu, turn on live captions”", L"“Solu, look at this”",
                                      L"“Headphones mode on”"};
    float x = left;
    for (const wchar_t* phrase : phrases) {
        const float w = std::ceil(gfx_.MeasureWidth(phrase, chipFont)) + 20.0f;
        if (x + w > right && x > left) { x = left; y += 28.0f; }
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.07f * alpha));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, y, x + w, y + 22), 11, 11}, brush_.Get());
        DrawText(dc, phrase, chipFont, D2D1::RectF(x, y, x + w, y + 22), WithAlpha(kText, 0.72f * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        x += w + 8.0f;
    }
}

// ---- the thin bar: Solu is awake, and it reacts to voices ---------------------------------------------------

void IslandWindow::DrawSoluBar(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar, float alpha) {
    if (!SoluAwake()) return;
    const float cx = (bar.left + bar.right) / 2, cy = (bar.top + bar.bottom) / 2;
    const bool thinking = solu_.state == SoluState::Thinking;
    const bool idle = solu_.state == SoluState::Ready;
    const D2D1_COLOR_F tint = thinking ? kAmber : kGreen;
    const float t = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;
    static const float kPattern[5] = {0.55f, 0.85f, 1.0f, 0.85f, 0.55f};
    for (int i = 0; i < 5; ++i) {
        float v;
        if (thinking) v = 0.5f + 0.5f * std::sin(t * 6.0f - i * 0.9f);              // dots chasing each other
        else v = Clamp01(barLevel_ * kPattern[i] * 1.4f);                            // follows the voice (yours, or Solu's)
        const float h = 1.8f + 3.2f * v;
        const float x = cx + (i - 2) * 4.6f;
        const float a = (idle && v < 0.05f ? 0.50f : 0.92f) * alpha;                 // quietly there while it waits
        brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x - 1.0f, cy - h / 2, x + 1.0f, cy + h / 2), 1.0f, 1.0f}, brush_.Get());
    }
}

// ---- mascot ----------------------------------------------------------------------------------

void IslandWindow::DrawMascot(float opacity) {
    mascot_->ComputeSkin(skin_);
    // Model space -> clip space for this window (row-vector convention).
    const float k = kMascotK * scale_;
    const D2D1_POINT_2F o = MascotPoint(0, 0);
    const float W = static_cast<float>(pxW_), H = static_cast<float>(pxH_);
    const DirectX::XMFLOAT4X4 viewProj(2 * k / W, 0, 0, 0,
                                       0, 2 * k / H, 0, 0,
                                       0, 0, 0, 0,
                                       2 * o.x * scale_ / W - 1, 1 - 2 * o.y * scale_ / H, 0.5f, 1);
    // Clip to the panel so spring overshoot or the rising entrance never draws outside it.
    const D2D1_RECT_F r = IslandRect();
    const RECT clip{static_cast<LONG>(r.left * scale_), static_cast<LONG>(r.top * scale_),
                    static_cast<LONG>(std::ceil(r.right * scale_)), static_cast<LONG>(std::ceil(r.bottom * scale_))};
    mascotRenderer_->Draw(gfx_.d3dContext(), rtv_.Get(), pxW_, pxH_, clip, viewProj, skin_, mascot_->Face(), opacity);
}

void IslandWindow::DrawMascotEffects(ID2D1DeviceContext* dc, float opacity) {
    using State = Mascot::State;
    const float t = mascot_->time();
    const float st = mascot_->stateTime();
    constexpr float kTau = 6.2831853f;

    // Music: a few notes drift up and away from the head, each on its own phase.
    if (mascot_->state() == State::Music) {
        IDWriteTextFormat* fmt = gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const wchar_t* glyphs[3] = {L"♪", L"♫", L"♪"};
        const D2D1_POINT_2F from = MascotPoint(0.36f, 0.16f);
        const float fadeIn = std::min(1.0f, st / 0.6f);
        for (int i = 0; i < 3; ++i) {
            const float phase = std::fmod(t * 0.55f + i / 3.0f, 1.0f);
            const float x = from.x + phase * 18.0f + std::sin(phase * kTau + i) * 3.0f;
            const float y = from.y - phase * 26.0f;
            const float a = std::sin(phase * 3.14159f) * 0.85f * fadeIn * opacity;
            DrawText(dc, glyphs[i], fmt, D2D1::RectF(x - 10, y - 10, x + 10, y + 10), D2D1::ColorF(0.80f, 0.76f, 1.0f, a),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // AI permission: a small "?" bobbing above the head, popping in when the request arrives.
    if (mascot_->state() == State::Permission) {
        const float pop = std::min(1.0f, st / 0.25f) * opacity;
        const float bob = std::sin(t * 3.2f) * 1.6f;
        const D2D1_POINT_2F at = MascotPoint(-0.40f, 0.30f);
        IDWriteTextFormat* fmt = gfx_.Text(18.0f, DWRITE_FONT_WEIGHT_BOLD);
        DrawText(dc, L"?", fmt, D2D1::RectF(at.x - 12, at.y - 13 + bob, at.x + 12, at.y + 13 + bob),
                 D2D1::ColorF(0.82f, 0.78f, 1.0f, 0.95f * pop), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Thinking: three dots pulse above the head, one after another.
    if (mascot_->talk() == Mascot::Talk::Thinking && mascot_->state() != State::Permission) {
        const D2D1_POINT_2F at = MascotPoint(-0.34f, 0.30f);
        for (int i = 0; i < 3; ++i) {
            const float phase = 0.5f + 0.5f * std::sin(t * 5.0f - i * 0.9f);
            brush_->SetColor(D2D1::ColorF(0.82f, 0.78f, 1.0f, (0.35f + 0.6f * phase) * opacity));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(at.x + (i - 1) * 9.0f, at.y - phase * 2.5f), 2.6f + phase * 0.8f, 2.6f + phase * 0.8f), brush_.Get());
        }
    }

    // Notification badge on the right ear; springs in when a notification lands.
    if (const int count = mascot_->badge(); count > 0) {
        float s = 1.0f;
        if (mascot_->state() == State::Notify) {            // ease-out-back pop
            const float u = std::min(1.0f, st / 0.35f) - 1.0f;
            s = 1.0f + 2.7f * u * u * u + 1.7f * u * u;
        }
        const D2D1_POINT_2F c = MascotPoint(0.27f, 0.25f);
        const float r = 8.0f * s;
        brush_->SetColor(D2D1::ColorF(1.0f, 0.271f, 0.227f, opacity));   // system red
        dc->FillEllipse(D2D1::Ellipse(c, r, r), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.9f * opacity));
        dc->DrawEllipse(D2D1::Ellipse(c, r, r), brush_.Get(), 0.8f);
        const std::wstring label = count > 9 ? L"9+" : std::to_wstring(count);
        DrawText(dc, label.c_str(), gfx_.Text(10.0f, DWRITE_FONT_WEIGHT_BOLD), D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r),
                 D2D1::ColorF(1, 1, 1, s > 0.2f ? opacity : 0.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

// ---- window procedure ------------------------------------------------------------------------

LRESULT CALLBACK IslandWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = static_cast<IslandWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<IslandWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT IslandWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;               // clicking the island never steals focus
    case WM_MOUSEMOVE:
        if (seeking_) {                                    // dragging the progress bar: follow the pointer, even outside it
            seekFraction_ = SeekFractionAt(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}).x, IslandRect());
            Animate();
            return 0;
        }
        OnMouseMove(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (expanded_) {
            if (!Pinned()) SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);
            if (hover_ != -1) { hover_ = -1; Animate(); }
        } else {
            KillTimer(hwnd_, kOpenTimer);
            openPending_ = false;
        }
        return 0;
    case WM_MOUSEWHEEL:
        OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_RBUTTONUP: {                                           // right-click a round button: quick mute / unmute
        const int hit = HitTest(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        if (expanded_ && hit >= kHitAudioButton && hit < kHitAudioButton + 3) ToggleMute(hit - kHitAudioButton);
        return 0;
    }
    case WM_LBUTTONDOWN:
        pressed_ = HitTest(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        if (pressed_ == kHitSeek) {
            seeking_ = true;
            seekFraction_ = SeekFractionAt(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}).x, IslandRect());
            Animate();
        }
        if (pressed_ >= 0) SetCapture(hwnd_);
        return 0;
    case WM_LBUTTONUP: {
        if (seeking_) {                                    // let go: jump there
            seekFraction_ = SeekFractionAt(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}).x, IslandRect());
            const double seconds = seekFraction_ * media_.duration;
            seeking_ = false;
            pressed_ = -1;
            if (GetCapture() == hwnd_) ReleaseCapture();
            media_.position = seconds;                     // show it right away; the app confirms a moment later
            media_.positionStamp = GetTickCount64();
            if (mediaHandler_) mediaHandler_(MediaCommand::Seek, seconds);
            Animate();
            return 0;
        }
        const int hit = HitTest(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        const int was = pressed_;
        pressed_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (was >= 0 && hit == was) OnClick(hit);
        return 0;
    }
    case WM_CAPTURECHANGED:
        pressed_ = -1;
        seeking_ = false;                                  // capture lost mid-drag: cancel, don't seek
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hover_ >= 0 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_TIMER:
        if (wp == kTestTimer) {
            KillTimer(hwnd_, kTestTimer);
            Animate();                                   // the test verdict has expired: show the normal status again
            return 0;
        }
        if (wp == kOpenTimer) {
            KillTimer(hwnd_, kOpenTimer);
            openPending_ = false;
            POINT pt;
            GetCursorPos(&pt);
            if (WindowFromPoint(pt) == hwnd_) Expand();   // still hovering after the intent delay
        } else if (wp == kCloseTimer || wp == kAutoCloseTimer) {
            KillTimer(hwnd_, wp);
            if (wp == kAutoCloseTimer) autoClosePending_ = false;
            if (!Pinned() && !CursorInIsland(kLeaveSlack)) Collapse();
        } else if (wp == kMascotTimer) {
            KillTimer(hwnd_, kMascotTimer);
            Animate();
        }
        return 0;
    case WM_APP_FRAME:
        Frame();
        return 0;
    case WM_DPICHANGED:
        // Our geometry follows the monitor, not the suggested rect; the app rebuilds islands on DPI change.
        PostMessageW(owner_, WM_DISPLAYCHANGE, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace eflow
