#include "island/IslandWindow.h"

#include <d2d1effects.h>
#include <shellapi.h>
#include <oleidl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <utility>

#include "core/Log.h"
#include "core/Messages.h"
#include "island/IslandStyle.h"
#include "island/MouseHook.h"
#include "mascot/MascotRenderer.h"

namespace eflow {
namespace {

// ---- design tokens (DIPs) --------------------------------------------------------------------
constexpr float kTopInset = 6.0f;          // gap between the screen edge and the island
constexpr float kMargin = 26.0f;           // room around the island for the shadow and spring overshoot
constexpr float kCollapsedW = 196.0f;
constexpr float kCollapsedH = 7.0f;
constexpr float kMaxRadius = 24.0f;
constexpr float kHotSlackX = 14.0f;        // extra hover area around the idle bar
constexpr float kHotSlackY = 9.0f;
constexpr float kLeaveSlack = 6.0f;        // how far outside the panel still counts as "inside"

constexpr float kTabPadX = 14.0f;
constexpr float kTabGap = 4.0f;

constexpr UINT kCloseDelayMs = 380;        // grace period before closing
constexpr UINT_PTR kHoldTimer = 1;         // a button held this long over the closed bar: it may be carrying files
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

constexpr D2D1_COLOR_F kIslandFill = Rgba(0.051f, 0.051f, 0.063f, 1.0f);

constexpr int kHitPrev = 10, kHitPlay = 11, kHitNext = 12;   // media control hit ids
constexpr int kHitShuffle = 14, kHitRepeat = 15;            // the two extra transport buttons
constexpr int kHitPopBack = 1040;                           // "< Voice": back to the settings list
constexpr int kHitCaption = 17;                             // the live conversation view: click to end it
constexpr int kHitOutput = 16;                              // the "playing on" chip (opens the speaker menu)
constexpr int kHitSeek = 13;                                // the progress bar (click or drag to seek)
constexpr int kHitReject = 20, kHitApprove = 21, kHitDefer = 22;  // permission card hit ids
constexpr int kHitRing = 40;                                 // + metric index (System tab)
constexpr float kMaxPanelH = 426.0f;                        // tallest the panel grows (the settings menu, 7 rows)
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
constexpr int kHitLive = 90, kHitLiveClose = 91;               // the live pill on the thin bar, and its close button
constexpr UINT_PTR kLiveTimer = 7;                          // repaints a live pill (countdown, shimmer)
constexpr float kLiveW = 352.0f, kLiveH = 32.0f;
constexpr uint64_t kArrivalMs = 900;                        // something plugged in: its pill's arrival animation
constexpr int kHitAgentTool = 3000;                         // + 0 Claude, + 1 Codex
constexpr int kHitAgentPrev = 3010, kHitAgentNext = 3011;    // flip between an agent's sessions
constexpr int kHitAgentGo = 3030;                           // the session header: go to the window where it runs
constexpr int kHitPrompt = 3020, kHitSend = 3021, kHitNewChat = 3022, kHitConnect = 3023;
constexpr UINT_PTR kAgentTimer = 8;
constexpr UINT_PTR kLyricsTimer = 9;                         // fires when the next lyric line starts
constexpr int kHitLyrics = 18;                               // the lyrics button in the Music tab
constexpr float kLyricPillH = 28.0f;                         // the closed island while a lyric line shows
constexpr float kLyricLinesH = 36.0f;                        // the two lyric lines in the Music tab                          // spinner and caret
constexpr float kAgentsH = 322.0f;                           // height of the island on the Agents tab
constexpr int kNotifIdsEnd = 2999;
constexpr int kHitTaskPause = 3400, kHitTaskStop = 3410;     // + row: pause / resume, stop / remove
constexpr float kSoluTasksH = 236.0f;
constexpr int kHitShelfItem = 3100;                          // + item index
constexpr int kHitShelfRemove = 3200;                        // + item index: its x button
constexpr int kHitShelfClear = 3300, kHitShelfAdd = 3301;
constexpr float kShelfH = 206.0f;
constexpr float kCardW = 76.0f, kCardH = 98.0f, kCardGap = 9.0f;
constexpr int kHitNotifClear = 30;                          // "Clear all"
constexpr int kHitNotifClose = 100;                         // + notification index: its dismiss button
constexpr float kNotifRowH = 44.0f;
constexpr float kNotifListTop = 54.0f;                      // from the panel top
constexpr float kNotifRows = 2.0f;                          // rows visible at once
constexpr float kArtSize = 92.0f;
constexpr float kArtTop = 62.0f;
constexpr float kControlsY = 145.0f;       // centre line of the transport buttons (from the panel top)

const wchar_t* const kTabLabels[kSectionCount] = {L"Music", L"Notifications", L"Solu AI", L"Agents", L"System", L"Shelf", L"Reminders",
                                                  L"SafeShare"};
const wchar_t* const kEmptyTitle[kSectionCount] = {L"Nothing playing", L"You're all caught up", L"No new messages",
                                                   L"No pending requests", L"", L"", L""};
const wchar_t* const kEmptyBody[kSectionCount] = {
    L"Play something in Spotify, YouTube or your browser",
    L"New notifications will show up here",
    L"WhatsApp messages will show up here",
    L"Requests from Solu, Claude Code and Codex appear here",
    L"",
    L"",
    L"",
};

std::wstring LowerCase(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}
// ---- System tab: what each ring can show -----------------------------------------------------
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

// The OLE drop target: files dragged from Explorer (or anywhere) onto the island. It only forwards to the window.
class FileDropTarget final : public IDropTarget {
public:
    explicit FileDropTarget(IslandWindow* window) : window_(window) {}
    void Detach() { window_ = nullptr; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        *effect = window_ ? window_->FilesDragEnter(data, {pt.x, pt.y}) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        *effect = window_ ? window_->FilesDragOver({pt.x, pt.y}) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (window_) window_->FilesDragLeave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        *effect = window_ ? window_->FilesDrop(data, {pt.x, pt.y}) : DROPEFFECT_NONE;
        return S_OK;
    }

private:
    IslandWindow* window_;
    ULONG refs_ = 1;
};

IslandWindow::~IslandWindow() {
    if (hwnd_) mousehook::Unregister(hwnd_);
    if (watching_ && systemWatchHandler_) systemWatchHandler_(false);
    if (audioWatching_ && audioWatchHandler_) audioWatchHandler_(false);
    if (hwnd_) {
        RevokeDragDrop(hwnd_);
        if (dropTarget_) {
            static_cast<FileDropTarget*>(dropTarget_)->Detach();
            dropTarget_->Release();
            dropTarget_ = nullptr;
        }
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
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                            kClassName, L"Eflow", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);              // (layered only so it can be click-through: fully opaque)
    mousehook::Register(hwnd_);

    UpdateScale();
    Place();
    if (!CreateSurface()) return false;

    // Selection pill starts on the first tab.
    const auto tabs = TabRects(D2D1::RectF(0, 0, kExpandedW, kExpandedH));
    tabX_.Snap(tabs[0].left);
    tabW_.Snap(tabs[0].right - tabs[0].left);

    SetHitRegion(false);
    dropTarget_ = new FileDropTarget(this);                            // files can be dropped on the island
    if (FAILED(RegisterDragDrop(hwnd_, dropTarget_))) Log("island: drag and drop is unavailable");
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

void IslandWindow::DebugSettings(int page) {
    debugPinned_ = page >= 0;
    if (page < 0) {
        OpenPopover(-1);
        Collapse();
        return;
    }
    Expand();
    OpenPopover(3);
    settingsPage_ = page;
    voiceScroll_ = 0;
    ApplyHeightTarget();
    Animate();
}

void IslandWindow::DebugDropPreview(int zone) {
    dragOver_ = zone >= 0;
    dropZone_ = zone == 1 ? DropZone::SafeShare : DropZone::Shelf;
    debugPinned_ = dragOver_;
    if (dragOver_) Expand();
    else Collapse();
    Animate();
}

void IslandWindow::DebugSnapshot(const std::wstring& path) {
    snapshotPath_ = path;                                              // written by the next frame, before it is shown
    Animate();
}

// The island's own pixels (never the screen behind it), as the frame about to be presented: for layout checks.
void IslandWindow::WriteSnapshot() {
    const std::wstring path = std::exchange(snapshotPath_, std::wstring());
    const D2D1_SIZE_U size = backbuffer_->GetPixelSize();
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Com<ID2D1Bitmap1> cpu;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, &props, &cpu)) || FAILED(cpu->CopyFromBitmap(nullptr, backbuffer_.Get(), nullptr)) ||
        FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
        Log("snapshot: could not read the frame");
        return;
    }
    ImageBGRA image;
    image.width = size.width;
    image.height = size.height;
    image.pixels.resize(static_cast<size_t>(size.width) * size.height * 4);
    for (UINT y = 0; y < size.height; ++y)
        memcpy(&image.pixels[static_cast<size_t>(y) * size.width * 4], mapped.bits + static_cast<size_t>(y) * mapped.pitch, size.width * 4);
    cpu->Unmap();
    Log(SavePng(image, path.c_str()) ? "snapshot: %ls" : "snapshot: could not write %ls", path.c_str());
}

std::string IslandWindow::DebugLayout() const {
    // Where things are once the panel has settled, in physical screen pixels (the shoot director clicks these).
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    const float winW = pxW_ / scale_;
    const D2D1_RECT_F open = D2D1::RectF((winW - kExpandedW) / 2.0f, TopInset(), (winW + kExpandedW) / 2.0f, TopInset() + h_.target);
    const D2D1_RECT_F bar = D2D1::RectF((winW - w_.target) / 2.0f, TopInset(), (winW + w_.target) / 2.0f, TopInset() + (expanded_ ? kCollapsedH : h_.target));
    std::string out = "{";
    auto pt = [&](const char* name, float x, float y) {
        char buf[128];
        sprintf_s(buf, "\"%s\":[%d,%d],", name, static_cast<int>(wr.left + x * scale_), static_cast<int>(wr.top + y * scale_));
        out += buf;
    };
    auto rc = [&](const char* name, const D2D1_RECT_F& r) { pt(name, (r.left + r.right) / 2, (r.top + r.bottom) / 2); };
    char name[32];
    rc("panel", open);
    rc("bar", bar);
    const auto tabs = TabRects(open);
    for (int i = 0; i < kSectionCount; ++i) { sprintf_s(name, "tab%d", i); rc(name, tabs[i]); }
    for (int i = 0; i < 4; ++i) { sprintf_s(name, "side%d", i); rc(name, AudioButton(open, i)); }
    const auto ctl = ControlCenters(open);
    pt("prev", ctl[0].x, ctl[0].y);
    pt("play", ctl[1].x, ctl[1].y);
    pt("next", ctl[2].x, ctl[2].y);
    const SeekBar seek = SeekGeometry(open);
    pt("seek0", seek.x0, seek.y);
    pt("seek1", seek.x1, seek.y);
    const D2D1_POINT_2F lyr = LyricsButton(open);
    pt("lyrics", lyr.x, lyr.y);
    rc("output", OutputChipRect(open, MediaOutput()));
    const auto perm = PermissionButtons(open);
    rc("reject", perm[0]);
    rc("approve", perm[1]);
    rc("defer", perm[2]);
    for (int i = 0; i < kMetricCount; ++i) { sprintf_s(name, "ring%d", i); rc(name, RingCell(open, i)); }
    for (size_t i = 0; i < notifications_.items.size() && i < 3; ++i) {
        sprintf_s(name, "notif%zu", i);
        rc(name, NotificationRow(open, i));
        sprintf_s(name, "notifx%zu", i);
        rc(name, NotificationClose(NotificationRow(open, i)));
    }
    if (popover_ >= 0) {
        const PopoverLayout g = PopoverGeometry(open);
        rc("mute", MuteSwitch(g));
        rc("test", TestPill(g, popover_));
        for (int i = 0; i < g.shown; ++i) {
            sprintf_s(name, "row%d", i);
            pt(name, (g.card.left + g.card.right) / 2, g.rowsTop + (i + 0.5f) * kPopRowH);
        }
    }
    const AgentsLayout ag = AgentsGeometry(open);
    rc("claude", ag.tool[0]);
    rc("codex", ag.tool[1]);
    rc("box", ag.box);
    rc("send", ag.send);
    rc("newchat", ag.newBtn);
    rc("shelf", ShelfArea(open));
    out.back() = '}';
    return out;
}
#endif

void IslandWindow::UpdateClickAway() {
    const bool want = CaptionsShown() && expanded_;
    if (want == clickAwayOn_) return;
    clickAwayOn_ = want;
    UpdateMouseHook();
}

bool IslandWindow::Pinned() const {
    // A pending AI permission request keeps the panel open until it is answered.
    return debugPinned_ || pressed_ >= 0 || field_ || (tourOn_ && expanded_) || !reminders_.ringing.empty() || dragOver_ || shelfDragging_ || (mascot_ && mascot_->permissionPending()) || CaptionsShown() ||
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
        const float x0 = (winW - CollapsedW()) / 2.0f - kHotSlackX;
        const float x1 = x0 + CollapsedW() + 2 * kHotSlackX;
        const float y1 = TopInset() + CollapsedH() + kHotSlackY;
        auto px = [&](float v) { return static_cast<int>(std::lround(v * scale_)); };
        rgn = CreateRectRgn(px(x0), 0, px(x1), px(y1));
    }
    SetWindowRgn(hwnd_, rgn, TRUE);   // the system owns rgn from here
    ApplyClickThrough();
    UpdateMouseHook();
}

void IslandWindow::ApplyClickThrough() {
    const bool through = !expanded_ && !holding_ && !TourInvite();   // (the tour's invitation opens on any click)
    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    const LONG_PTR want = through ? (ex | WS_EX_TRANSPARENT) : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if (want != ex) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, want);
}

void IslandWindow::UpdateMouseHook() {
    RECT closed{};                                               // where a middle click opens it (screen pixels)
    if (!expanded_ && !suppressed_) {
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        const float winW = pxW_ / scale_;
        const float x0 = (winW - CollapsedW()) / 2.0f - kHotSlackX, x1 = x0 + CollapsedW() + 2 * kHotSlackX;
        const float y1 = TopInset() + CollapsedH() + kHotSlackY;
        closed = {wr.left + std::lround(x0 * scale_), wr.top, wr.left + std::lround(x1 * scale_), wr.top + std::lround(y1 * scale_)};
    }
    mousehook::Update(hwnd_, closed, clickAwayOn_);
}

// ---- state machine ---------------------------------------------------------------------------

void IslandWindow::Expand() {
    if (expanded_ || suppressed_) return;
    expanded_ = true;
    if (tourOn_ && tourStep_ == static_cast<int>(TourStep::Invite)) {   // opened from the invitation: lesson one learned
        tourStep_ = tourSeen_ = std::max(tourSeen_, static_cast<int>(TourStep::Hello));
        tourTime_ = 0;
        tourSwap_ = 1;
        TourEnter(TourStep::Hello);
        Log("tour: opened from the invitation");
    }
    AutoPickAgent();
    captionsYielded_ = false;                    // opening again during a conversation shows the live view again
    KillTimer(hwnd_, kCloseTimer);
    UpdateSystemWatch();
    UpdateAudioWatch();
    SetOpenSprings(w_, h_);
    w_.target = kExpandedW;
    h_.target = TargetHeight();
    SetHitRegion(true);
    UpdateClickAway();
    Animate();
}

void IslandWindow::Collapse() {
    if (!expanded_) return;
    Blur(true);
    dragOver_ = dragOpened_ = false;
    shelfScroll_ = 0;
    agentAuto_ = true;                           // next time it opens on whichever agent is busy
    agentSession_.clear();
    agentScroll_ = 0;
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
    w_.target = CollapsedW();
    h_.target = CollapsedH();
    UpdateClickAway();
    Animate();
}

void IslandWindow::SetSuppressed(bool suppressed) {
    if (suppressed == suppressed_) return;
    suppressed_ = suppressed;
    if (suppressed) {
        KillTimer(hwnd_, kMascotTimer);       // nothing renders while hidden
        KillTimer(hwnd_, kAutoCloseTimer);
        KillTimer(hwnd_, kHoldTimer);
        KillTimer(hwnd_, kCloseTimer);
        holding_ = false;
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
        UpdateMouseHook();
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

int IslandWindow::MaxRows(int side) const { return side == 3 ? 7 : kPopMaxRows; }

void IslandWindow::SetSolu(const SoluSnapshot& snapshot) {
    const bool wasActive = SoluActive();
    const SoluState before = solu_.state;
    solu_ = snapshot;
    if (!SoluActive()) captionsYielded_ = false;                       // the next conversation shows the live view again
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
    } else if (((wasActive && !SoluActive()) || (before == SoluState::Loading && solu_.state != SoluState::Loading)) && expanded_ &&
               !Pinned() && !CursorInIsland(kLeaveSlack)) {          // (only on that change: a level update must not re-arm the close timer)
        SetTimer(hwnd_, kCloseTimer, 1800, nullptr);                   // the conversation is over: tuck away after a moment
    }
    ApplyHeightTarget();                                               // the live view grows with the answer
    UpdateClickAway();
    RefreshCollapsed();                                                // a conversation hides the lyric pill, and brings it back after
    if (!expanded_ && (Live() || MusicPill()) && solu_.state == before) return;   // a pill covers the bar: its voice meter is not on screen
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
    case State::Notify: OnNotification(1); break;
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

void IslandWindow::OnNotification(int count) {
    // Worth attention: open by itself on the Notifications tab, and close again after a moment unless
    // the user has the pointer on it (or it was already open).
    if (mascot_) mascot_->Notify();
    notifScrollTarget_ = 0;                      // newest first: show it
    if (!reminders_.ringing.empty()) return;     // a ringing reminder keeps the island until it is answered
    if (tourOn_) {                               // the welcome tour has the island: the message waits on its tab
        notifBadge_ += count;
        return;
    }
    const bool wasOpen = expanded_;
    if (!Present(Section::Notifications, false)) {        // the user is busy on another tab: only "+1" on its tab
        notifBadge_ += count;
        tabDot_[static_cast<size_t>(Section::Notifications)] = 0;   // (the count takes the dot's place)
        return;
    }
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
    if (GetTickCount64() - wheelOpenedAt_ < 400) return;          // still the turn of the wheel that opened the island
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
    if (expanded_ && section_ == Section::Shelf) {                          // slide along the shelf
        const D2D1_RECT_F area = ShelfArea(IslandRect());
        const float total = (static_cast<float>(shelf_.items.size()) + 1) * (kCardW + kCardGap);
        shelfScroll_ = std::clamp(shelfScroll_ - (delta > 0 ? 1.0f : -1.0f) * (kCardW + kCardGap) * 1.5f, 0.0f, std::max(0.0f, total - (area.right - area.left)));
        Animate();
        return;
    }
    if (expanded_ && section_ == Section::Reminders && reminders_.ringing.empty()) {   // scroll a long reminder list
        const int rows = RemindersGeometry(IslandRect()).rows;
        reminderScroll_ = std::clamp(reminderScroll_ + (delta > 0 ? -1 : 1), 0, std::max(0, static_cast<int>(reminders_.items.size()) - rows));
        Animate();
        return;
    }
    if (expanded_ && section_ == Section::SafeShare && shareSentView_) {    // scroll the rooms sent
        shareScroll_ = std::clamp(shareScroll_ + (delta > 0 ? -1 : 1), 0,
                                  std::max(0, static_cast<int>(shareSnap_.sent.size()) - ShareGeometry(IslandRect()).rows));
        Animate();
        return;
    }
    if (expanded_ && section_ == Section::Ai && permissions_.empty()) {      // scroll the agent's feed
        if (const AgentSession* s = CurrentSession())
            agentScroll_ = std::clamp(agentScroll_ + (delta > 0 ? 1 : -1), 0, std::max(0, static_cast<int>(s->feed.size()) - 1));
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
        const bool yielded = captionsYielded_;
        const bool now = Present(Section::Ai, true);
        captionsYielded_ = yielded;              // not the user's choice: the live view comes back after the card
        if (now) Expand();
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
    if (TourShown()) return TourHit(p, island);
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
        if (popover_ == 3 && settingsPage_ != 0 && inside(D2D1::RectF(g.card.left - 6, g.card.top - 4, g.card.left + 130, g.card.top + 28))) return kHitPopBack;
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
        if (p.x >= c.right - 50 && p.x <= c.right + 4 && p.y >= c.top - 5 && p.y <= c.top + 21) return kHitCaption;
    }
    if (section_ == Section::System) {
        for (int i = 0; i < kMetricCount; ++i) {
            const D2D1_RECT_F cell = RingCell(island, i);
            if (p.x >= cell.left && p.x <= cell.right && p.y >= cell.top && p.y <= cell.bottom) return kHitRing + i;
        }
    }
    if (section_ == Section::Solu && solu_.state == SoluState::Missing) {
        const float left = island.left + 26, right = island.left + kExpandedW - (mascot_ ? kMascotLane : 26.0f);
        const float top = island.top + kTabTop + kTabH + 14.0f;
        if (p.x >= left && p.x <= right && p.y >= top && p.y <= top + 40.0f) return kHitSoluSetup;
    }
    if (section_ == Section::Solu && SoluTaskRows() > 0 && !CaptionsShown()) {
        const float right = island.left + kExpandedW - kMascotLane;
        for (int i = 0; i < SoluTaskRows(); ++i) {
            const float top = island.top + kTabTop + kTabH + 14.0f + 56.0f + i * 40.0f;
            if (p.y < top || p.y > top + 38) continue;
            const SoluTask& t = solu_.tasks[static_cast<size_t>(i)];
            const bool live = t.status == L"running" || t.status == L"waiting" || t.status == L"planning" || t.status == L"paused";
            if (p.x >= right - 30 && p.x <= right + 4) return kHitTaskStop + i;
            if (live && p.x >= right - 62 && p.x < right - 32) return kHitTaskPause + i;
        }
    }
    if (section_ == Section::Shelf) {
        const D2D1_RECT_F area = ShelfArea(island);
        if (p.y < area.top - 4 && p.y > island.top + 50) {                 // the header row: "Clear"
            if (!shelf_.items.empty() && p.x > area.right - 60 && p.y < area.top - 4) return kHitShelfClear;
            return -1;
        }
        if (p.x < area.left || p.x > area.right || p.y < area.top || p.y > area.bottom) return -1;
        const int n = static_cast<int>(shelf_.items.size());
        for (int i = 0; i <= n; ++i) {
            const D2D1_RECT_F c = ShelfCard(island, i);
            if (c.right < area.left || c.left > area.right) continue;
            if (p.x < c.left || p.x > c.right || p.y < c.top || p.y > c.bottom) continue;
            if (i == n) return kHitShelfAdd;
            if (p.x > c.right - 22 && p.y < c.top + 22) return kHitShelfRemove + i;
            return kHitShelfItem + i;
        }
        if (n == 0) return kHitShelfAdd;                                   // the whole empty drop zone
        return -1;
    }
    if (section_ == Section::Reminders) return RemindersHit(p, island);
    if (section_ == Section::SafeShare) return SafeShareHit(p, island);
    if (section_ == Section::Ai && permissions_.empty()) {          // the agents panel
        const AgentsLayout g = AgentsGeometry(island);
        auto in = [&](const D2D1_RECT_F& r) { return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom; };
        for (int i = 0; i < 2; ++i)
            if (in(g.tool[i])) return kHitAgentTool + i;
        const auto mine = SessionsOf(agentTool_);
        if (mine.size() > 1) {
            if (in(g.prev)) return kHitAgentPrev;
            if (in(g.next)) return kHitAgentNext;
        }
        const bool hooked = agentTool_ == AgentTool::Claude ? agents_.claudeHooked : agents_.codexHooked;
        if (!hooked && in(g.connect)) return kHitConnect;
        if (CurrentSession() && p.x >= g.header.left && p.x <= g.header.right - 150 && p.y >= g.header.top - 2 && p.y <= g.header.bottom + 2)
            return kHitAgentGo;
        if (in(g.newBtn)) return kHitNewChat;
        if (in(g.send)) return kHitSend;
        if (in(g.box)) return kHitPrompt;
        return -1;
    }
    if (section_ == Section::Ai && !permissions_.empty()) {
        const auto buttons = PermissionButtons(island);
        for (int i = 0; i < 3; ++i)
            if (p.x >= buttons[i].left && p.x <= buttons[i].right && p.y >= buttons[i].top && p.y <= buttons[i].bottom &&
                !(i == 2 && permissions_.front().id >= kSoluApprovalBase && !permissions_.front().canAlways))   // no third choice
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
        const D2D1_POINT_2F lb = LyricsButton(island);
        if ((p.x - lb.x) * (p.x - lb.x) + (p.y - lb.y) * (p.y - lb.y) <= 16.0f * 16.0f) return kHitLyrics;
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
    if (!expanded_) return;                      // (closed it only takes the pointer while files are carried over it)
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

bool IslandWindow::UserBusy() const {
    return expanded_ && (field_ || popover_ >= 0 || CursorInIsland(kLeaveSlack));
}

bool IslandWindow::Present(Section section, bool important) {
    const int id = static_cast<int>(section);
    if (!UserBusy() || section_ == section) {
        OnClick(id);
        return true;
    }
    tabDot_[static_cast<size_t>(id)] = std::max(tabDot_[static_cast<size_t>(id)], important ? 2 : 1);
    if (important) pendingSection_ = id;
    Animate();
    return false;
}

void IslandWindow::ShowPending() {
    if (pendingSection_ < 0) return;
    const int id = pendingSection_;
    pendingSection_ = -1;
    const bool stillNeeded = (id == static_cast<int>(Section::Ai) && !permissions_.empty()) ||
                             (id == static_cast<int>(Section::Reminders) && !reminders_.ringing.empty()) ||
                             (id == static_cast<int>(Section::SafeShare) && shareSnap_.phone.state == PhoneState::Asking);
    if (stillNeeded) OnClick(id);                // (answered meanwhile, by voice or in the tool: nothing to show)
}

void IslandWindow::OnClick(int id) {
    if (id >= kHitTourFirst && id <= kHitTourLast) {
        TourClick(id);
        Animate();
        return;
    }
    if (id == kHitSoluSetup) {
        StartTour(TourStep::Solu);                   // the guided setup is the tour's Solu step
        hover_ = -1;
        Animate();
        return;
    }
    if (id >= kHitReminderFirst && id <= kHitReminderLast) {
        RemindersClick(id);
        hover_ = -1;
        Animate();
        return;
    }
    if (id >= kHitShareFirst && id <= kHitShareLast) {
        SafeShareClick(id);
        Animate();
        return;
    }
    if (id >= kHitTaskPause && id < kHitTaskStop + 10) {
        const bool stop = id >= kHitTaskStop;
        const size_t i = static_cast<size_t>(id - (stop ? kHitTaskStop : kHitTaskPause));
        if (i < solu_.tasks.size() && taskHandler_) {
            const SoluTask& t = solu_.tasks[i];
            const bool live = t.status == L"running" || t.status == L"waiting" || t.status == L"planning" || t.status == L"paused";
            taskHandler_(stop ? (live ? "cancel" : "remove") : (t.status == L"paused" ? "resume" : "pause"), t.id);
        }
        hover_ = -1;
        Animate();
        return;
    }
    if (id >= kHitShelfItem && id <= kHitShelfAdd) {
        const int n = static_cast<int>(shelf_.items.size());
        if (id >= kHitShelfItem && id < kHitShelfItem + 100) {
            const int i = id - kHitShelfItem;
            if (i < n) {
                const std::wstring& path = shelf_.items[static_cast<size_t>(i)].path;
                const ULONGLONG now = GetTickCount64();
                if (shelfLastItem_ == i && now - shelfLastClick_ < GetDoubleClickTime()) {   // a double click opens it
                    OpenShelfItem(i, false);
                    shelfLastItem_ = -1;
                } else if (GetKeyState(VK_CONTROL) & 0x8000) {
                    if (!shelfSel_.erase(path)) shelfSel_.insert(path);
                    shelfLastItem_ = i, shelfLastClick_ = now;
                } else {
                    const bool only = shelfSel_.size() == 1 && shelfSel_.count(path);
                    shelfSel_.clear();
                    if (!only) shelfSel_.insert(path);
                    shelfLastItem_ = i, shelfLastClick_ = now;
                }
            }
        } else if (id >= kHitShelfRemove && id < kHitShelfRemove + 100) {
            const int i = id - kHitShelfRemove;
            if (i < n && shelfRemove_) {
                shelfSel_.erase(shelf_.items[static_cast<size_t>(i)].path);
                shelfRemove_(shelf_.items[static_cast<size_t>(i)].path);
            }
        } else if (id == kHitShelfClear && shelfClear_) {
            shelfSel_.clear();
            shelfClear_();
        } else if (id == kHitShelfAdd) {
            ShelfAddFiles();
        }
        hover_ = -1;
        Animate();
        return;
    }
    if (id >= kHitAgentTool && id < kHitAgentTool + 40) {
        if (id < kHitAgentTool + 2) {
            agentTool_ = id == kHitAgentTool ? AgentTool::Claude : AgentTool::Codex;
            agentAuto_ = false;
            agentSession_.clear();
            agentScroll_ = 0;
        } else if (id == kHitAgentPrev || id == kHitAgentNext) {
            const auto mine = SessionsOf(agentTool_);
            if (!mine.empty()) {
                size_t at = 0;
                for (size_t i = 0; i < mine.size(); ++i)
                    if (mine[i]->id == (agentSession_.empty() ? mine[0]->id : agentSession_)) at = i;
                at = (at + (id == kHitAgentNext ? 1 : mine.size() - 1)) % mine.size();
                agentSession_ = mine[at]->id;
                agentAuto_ = false;
                agentScroll_ = 0;
            }
        } else if (id == kHitAgentGo) {
            if (const AgentSession* s = CurrentSession(); s && agentOpen_) agentOpen_(*s, (GetKeyState(VK_CONTROL) & 0x8000) != 0);   // Ctrl: also reload the chat
        } else if (id == kHitPrompt) {
            Focus(prompt_);
        } else if (id == kHitSend) {
            SendPrompt();
        } else if (id == kHitNewChat) {
            newChat_ = !newChat_;
        } else if (id == kHitConnect && agentConnect_) {
            agentConnect_(agentTool_);
        }
        Animate();
        return;
    }
    if (id == kHitLive) {
        const std::wstring front = FrontGlimpse();
        const bool agent = front.rfind(L"agent", 0) == 0, task = front.rfind(L"task", 0) == 0, shot = front.rfind(L"shot:", 0) == 0;
        const bool shared = front.rfind(L"share:", 0) == 0;
        Expand();
        if (agent) OnClick(static_cast<int>(Section::Ai));             // a pill about an agent: straight to its tab
        else if (task) OnClick(static_cast<int>(Section::Solu));       // a task: the Solu tab lists it
        else if (shot) {                                               // a screenshot: on the Shelf, ready to drag anywhere
            OnClick(static_cast<int>(Section::Shelf));
            if (activityDismiss_) activityDismiss_(front);            // (its pill has done its job)
        }
        else if (shared) {                                             // a sent room was opened: the list of sent rooms
            OnClick(static_cast<int>(Section::SafeShare));
            shareSentView_ = true;
            if (activityDismiss_) activityDismiss_(front);
        }
        else if (front == L"music") OnClick(static_cast<int>(Section::Music));   // the music pill: the song
        return;
    }
    if (id == kHitLiveClose) {
        if (const Activity* shown = FrontActivity()) {
            const std::wstring gone = shown->id;
            std::vector<Activity> rest;
            for (const Activity& a : activities_)
                if (a.id != gone) rest.push_back(a);
            if (activityDismiss_) activityDismiss_(gone);
            SetActivities(rest);
        }
        return;
    }
    if ((id >= 0 && id < kSectionCount) || (id >= kHitAudioButton && id < kHitAudioButton + 4)) {
        captionsYielded_ = true;                                  // tabs and the round buttons always work, even mid-conversation
        Log("island: live view yielded (click id %d)", id);
        UpdateClickAway();
        ApplyHeightTarget();
    }
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
    if (id >= kHitPopRow && id < kHitPopRow + static_cast<int>(popRowHover_.size())) {
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
                } else if (k == 3) {                               // screen help
                    solu_.screen = !solu_.screen;
                    if (soluScreen_) soluScreen_(solu_.screen);
                } else if (k >= 4 && k <= 6) {                     // Voice >, Notifications >, Closed island >
                    settingsPage_ = k == 4 ? 1 : k == 5 ? 2 : 3;
                    voiceScroll_ = 0;
                    ApplyHeightTarget();
                }
            } else if (settingsPage_ == 3) {                       // what the closed island may show
                if (k == 0) glimpseLyrics_ = !glimpseLyrics_;
                else if (k == 1) glimpseAgents_ = !glimpseAgents_;
                if (glimpseHandler_) glimpseHandler_(glimpseLyrics_, glimpseAgents_);
                RefreshCollapsed();
            } else if (settingsPage_ == 2) {                       // mute all, or one app
                const auto apps = MuteRows();
                if (k == 0) {
                    mutes_.all = !mutes_.all;
                    if (muteAll_) muteAll_(mutes_.all);
                } else if (!mutes_.all && k - 1 < static_cast<int>(apps.size())) {   // (with "Mute all" on, every app already is)
                    const MutedApp& app = apps[static_cast<size_t>(k - 1)];
                    const bool muted = !mutes_.IsMuted(app.id, app.name);
                    mutes_.Set(app.id, app.name, muted);
                    if (muteApp_) muteApp_(app.id, app.name, muted);
                }
                ApplyHeightTarget();
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
        if (GetTickCount64() - cardShownAt_ < 700) {               // it just appeared: a click meant for the last one must not answer it
            Log("island: click on a card that just appeared, ignored");
            return;
        }
        Log("island: approval card '%ls %ls' -> %s", permissions_.front().source.c_str(), permissions_.front().action.c_str(),
            id == kHitApprove ? "Approve" : id == kHitReject ? "Reject" : "third button");
        const PermissionRequest& front = permissions_.front();
        // A question, or "Answer in ...": step aside and bring up the window it is waiting in.
        if (front.isQuestion || id == kHitDefer) {
            const AgentSession* host = nullptr;
            for (const AgentSession& s : agents_.sessions) {
                if ((front.source == L"Claude Code") != (s.tool == AgentTool::Claude)) continue;
                if (!front.session.empty() && s.id == front.session) { host = &s; break; }
                if (host) continue;
                if (front.cwd.empty() || s.cwd == front.cwd || s.project == front.project) host = &s;
            }
            if (host && agentOpen_) agentOpen_(*host, false);
        }
        const PermissionDecision d = id == kHitApprove ? PermissionDecision::Allow
                                   : id == kHitReject  ? PermissionDecision::Deny
                                                       : PermissionDecision::Defer;
        const uint64_t answered = front.id;
        permissions_.erase(permissions_.begin());     // the server confirms with a fresh list right away
        permissionHandler_(answered, d);              // (after the erase: that list may arrive inside this call)
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
            const uint32_t gone = notifications_.items[i].id;
            notifications_.items.erase(notifications_.items.begin() + static_cast<std::ptrdiff_t>(i));
            dismissHandler_(gone);                 // after the erase: the handler may hand us a fresh list at once
        }
        notifScrollTarget_ = std::clamp(notifScrollTarget_, 0.0f,
                                        std::max(0.0f, static_cast<float>(notifications_.items.size()) - kNotifRows));
        hover_ = -1;
        Animate();
        return;
    }
    if (id == kHitLyrics) {
        lyricsOn_ = !lyricsOn_;
        if (lyricsHandler_) lyricsHandler_(lyricsOn_);
        UpdateLyricLine();
        ApplyHeightTarget();
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
    if (field_ && FieldSection(field_) != id) Blur(false);   // the box being typed in is not on the new tab
    tabDot_[static_cast<size_t>(id)] = 0;
    if (id == static_cast<int>(Section::Notifications)) notifBadge_ = 0;
    if (pendingSection_ == id) pendingSection_ = -1;
    section_ = static_cast<Section>(id);
    popover_ = -1;                               // switching tabs dismisses an open menu
    UpdateSystemWatch();
    UpdateAudioWatch();
    UpdateAgentTimer();
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
    } else if (tourOn_ && !reducedMotion_) {    // the tour: its scenes play (the closed invitation glows at a gentler pace)
        SetTimer(hwnd_, kMascotTimer, expanded_ ? kMascotActiveMs : kMascotCalmMs, nullptr);
    } else if (mascotVisible) {                 // open and still: only the mascot moves, at a gentler cadence
        SetTimer(hwnd_, kMascotTimer, mascot_->calm() ? kMascotCalmMs : kMascotActiveMs, nullptr);
    }                                           // closed and still: no frames at all
    if (panelMoving_ && !moving) OnSettled();
    panelMoving_ = moving;
    // Windows serves posted messages (our next frame) before mouse input, and makes WM_TIMER only when the queue is
    // empty. A frame loop that never stops (Solu talking or listening) would starve both: hover, clicks and
    // close-on-leave would wait until the voice stops. So deliver pending input and due timers between frames.
    if (moving) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE | PM_QS_INPUT)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        while (PeekMessageW(&m, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) DispatchMessageW(&m);
    }
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
    moving |= TourAnimate(dt);
    moving |= Approach(dragT_, dragOver_ ? 1.0f : 0.0f, dt, 14.0f);
    moving |= Approach(dropShare_, dropZone_ == DropZone::SafeShare ? 1.0f : 0.0f, dt, 18.0f);
    if (dragOver_) moving = true;                                         // the zone under the files breathes
    if (expanded_ && section_ == Section::Shelf) {
        shelfHover_.resize(shelf_.items.size(), 0.0f);
        for (size_t i = 0; i < shelf_.items.size(); ++i) {
            const int hv = hover_;
            const bool on = hv == kHitShelfItem + static_cast<int>(i) || hv == kHitShelfRemove + static_cast<int>(i);
            moving |= Approach(shelfHover_[i], on ? 1.0f : 0.0f, dt, 20.0f);
            float& pop = shelfPop_[shelf_.items[i].path];
            moving |= Approach(pop, 1.0f, dt, 12.0f);
        }
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
    moving |= Approach(captionEndHover_, hover_ == kHitCaption ? 1.0f : 0.0f, dt, 22.0f);
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
    moving |= Approach(lyricsHover_, hover_ == kHitLyrics ? 1.0f : 0.0f, dt, 22.0f);
    moving |= Approach(lyricT_, 1.0f, dt, 9.0f);                  // a lyric line rises in (~0.35 s), then no more frames
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
    const float mascotAlpha = mascot_ && !suppressed_ ? ContentAlpha() * (1.0f - dragT_) : 0.0f;   // (it steps aside for a drop)
    if (SUCCEEDED(hr) && mascotAlpha > 0.003f) {
        DrawMascot(mascotAlpha);
        dc_->BeginDraw();
        DrawMascotEffects(dc_.Get(), mascotAlpha);
        hr = dc_->EndDraw();
    }
#ifndef NDEBUG
    if (SUCCEEDED(hr) && !snapshotPath_.empty()) WriteSnapshot();
#endif
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

    if (progress < 0.34f) {
        const float liveIn = Clamp01((h_.x - kCollapsedH) / (kLiveH - kCollapsedH));
        if (tourOn_ && (!expanded_ || progress < 0.2f)) {                       // the tour's invitation (fading as it opens)
            DrawTourInvite(dc, r, liveIn * (expanded_ ? 1.0f - progress / 0.2f : 1.0f - progress / 0.34f));
        } else {
            if ((Live() || MusicPill()) && !expanded_) DrawGlimpseIcons(dc, r, (1.0f - progress / 0.34f));
            if (Live() && !expanded_) DrawLive(dc, r, liveIn * (1.0f - progress / 0.34f));
            else if (Live() && expanded_ && progress < 0.2f) DrawLive(dc, r, (1.0f - progress / 0.2f) * liveIn);
            else if (MusicPill() && !expanded_)
                DrawMusicPill(dc, r, Clamp01((h_.x - kCollapsedH) / (kLyricPillH - kCollapsedH)) * (1.0f - progress / 0.34f));
            else DrawSoluBar(dc, r, 1.0f - progress / 0.34f);                    // Solu is awake: the bar shows it
        }
    }
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
    if (TourShown()) {
        DrawTour(dc, D2D1::RectF(base.left, base.top, base.right, std::max(base.bottom, island.bottom + drift)), alpha);
        return;
    }
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
        if (i == static_cast<int>(Section::Solu)) {   // status dot, same colours as the buttons' lights: green awake, amber busy, grey off
            labelRect.left += 12.0f;
            const bool busy = solu_.state == SoluState::Loading || solu_.state == SoluState::Thinking;
            const D2D1_COLOR_F c = busy ? kAmber : SoluAwake() ? kGreen : kNeutral;
            brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, std::min(a + 0.2f, 1.0f) * alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tabs[i].left + 12.0f, (tabs[i].top + tabs[i].bottom) / 2), 3.0f, 3.0f), brush_.Get());
        }
        DrawText(dc, kTabLabels[i], tabFont, labelRect, WithAlpha(kText, std::min(a, 0.96f) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (i == static_cast<int>(Section::Notifications) && notifBadge_ > 0) {   // "+3": new while the user was elsewhere
            const std::wstring count = L"+" + std::to_wstring(std::min(notifBadge_, 99));
            IDWriteTextFormat* badgeFont = gfx_.Text(9.5f, DWRITE_FONT_WEIGHT_BOLD);
            const float w = std::max(16.0f, std::ceil(gfx_.MeasureWidth(count.c_str(), badgeFont)) + 8.0f);
            const D2D1_RECT_F badge = D2D1::RectF(tabs[i].right - w + 4.0f, tabs[i].top - 3.0f, tabs[i].right + 4.0f, tabs[i].top + 11.0f);
            brush_->SetColor(D2D1::ColorF(kRed.r, kRed.g, kRed.b, alpha));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{badge, 7.0f, 7.0f}, brush_.Get());
            DrawText(dc, count.c_str(), badgeFont, badge, D2D1::ColorF(1, 1, 1, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (tabDot_[static_cast<size_t>(i)] > 0) {               // news on a tab the user is not on
            const D2D1_COLOR_F c = tabDot_[static_cast<size_t>(i)] == 2 ? kAmber : kAccent;
            brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tabs[i].right - 6.0f, tabs[i].top + 7.0f), 3.2f, 3.2f), brush_.Get());
        }
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
        if (i == static_cast<int>(Section::Shelf)) {
            DrawShelf(dc, D2D1::RectF(base.left, base.top + slide, base.right, std::max(base.bottom, island.bottom + drift) + slide), a);
            continue;
        }
        if (i == static_cast<int>(Section::Reminders)) {
            DrawReminders(dc, D2D1::RectF(base.left, base.top + slide, base.right, std::max(base.bottom, island.bottom + drift) + slide), a);
            continue;
        }
        if (i == static_cast<int>(Section::SafeShare)) {
            DrawSafeShare(dc, D2D1::RectF(base.left, base.top + slide, base.right, std::max(base.bottom, island.bottom + drift) + slide), a);
            continue;
        }
        if (i == static_cast<int>(Section::Ai)) {
            DrawAgents(dc, D2D1::RectF(base.left, base.top + slide, base.right, std::max(base.bottom, island.bottom + drift) + slide), a);
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
    DrawSoluCaption(dc, D2D1::RectF(base.left, base.top, base.right, std::max(base.bottom, island.bottom + drift)), alpha);   // the live view may be taller than a normal tab
    DrawAudioPopover(dc, base, alpha);
    DrawDropChooser(dc, island, alpha);                              // files hovering: where shall they go (on top of all)
}

// ---- music -----------------------------------------------------------------------------------

IslandWindow::SeekBar IslandWindow::SeekGeometry(const D2D1_RECT_F& panel) const {
    // The same numbers DrawMusic uses: left edge after the artwork, right edge before the mascot's lane.
    return {panel.left + 24 + kArtSize + 18, panel.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f), panel.top + 113 + LyricShift() + 1.5f};
}

float IslandWindow::SeekFractionAt(float x, const D2D1_RECT_F& panel) const {
    const SeekBar bar = SeekGeometry(panel);
    return Clamp01((x - bar.x0) / std::max(1.0f, bar.x1 - bar.x0));
}

void IslandWindow::SetMedia(const MediaInfo& media) {
    media_ = media;
    UpdateLyricLine();
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
    const float cx = (x0 + x1) / 2.0f, cy = island.top + kControlsY + LyricShift();
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
    if (LyricShift() > 0) DrawLyricLines(dc, x0, x1, base.top + 106, alpha);
    const float barY = base.top + 113 + LyricShift();
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
    {                                                                // lyrics: green when showing, quiet when this song has none
        const D2D1_POINT_2F c = LyricsButton(D2D1::RectF(base.left, base.top, base.right, base.bottom));
        if (lyricsHover_ > 0) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * lyricsHover_ * alpha));
            dc->FillEllipse(D2D1::Ellipse(c, 14, 14), brush_.Get());
        }
        const bool showing = LyricsReady();
        const float level = showing ? 0.98f : lyricsOn_ ? 0.45f : 0.30f;
        DrawText(dc, L"\uE8BD", smallIcons, D2D1::RectF(c.x - 14, c.y - 14, c.x + 14, c.y + 14),
                 WithAlpha(showing ? kGreen : kText, std::min(1.0f, level + 0.3f * lyricsHover_) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (lyricsOn_) {
            brush_->SetColor(WithAlpha(showing ? kGreen : kText, (showing ? 0.95f : 0.35f) * alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y + 13), 1.7f, 1.7f), brush_.Get());
        }
    }
}

// ---- AI permission requests -------------------------------------------------------------------

void IslandWindow::SetPermissions(const std::vector<PermissionRequest>& pending) {
    permissions_ = pending;
    if (!permissions_.empty() && permissions_.front().id != cardId_) {   // a different card is now in front
        cardId_ = permissions_.front().id;
        cardShownAt_ = GetTickCount64();
    }
    SetPermissionPending(!permissions_.empty());   // opens on the AI tab for a new request; closes when done
    ApplyHeightTarget();
    Animate();
}

void IslandWindow::SetNotificationMutes(const NotificationMutes& mutes) {
    mutes_ = mutes;
    if (popover_ == 3 && settingsPage_ == 2) ApplyHeightTarget();
    Animate();
}

std::vector<MutedApp> IslandWindow::MuteRows() const {
    std::vector<MutedApp> rows;
    auto add = [&](const std::wstring& id, const std::wstring& name) {
        const std::wstring key = id.empty() ? name : id;
        if (!name.empty() && std::none_of(rows.begin(), rows.end(), [&](const MutedApp& r) { return r.id == key; })) rows.push_back({key, name});
    };
    for (const NotificationItem& n : notifications_.items) add(n.appId, n.app);   // the apps sending notifications now
    for (const MutedApp& a : mutes_.apps) add(a.id, a.name);                      // ... and the muted ones, even when quiet
    return rows;
}

void IslandWindow::SetReminders(const ReminderSnapshot& snapshot) {
    const bool newAlert = std::any_of(snapshot.ringing.begin(), snapshot.ringing.end(), [&](const Alert& a) {
        return std::none_of(reminders_.ringing.begin(), reminders_.ringing.end(), [&](const Alert& b) { return a.id == b.id && a.early == b.early; });
    });
    const bool wasRinging = !reminders_.ringing.empty();
    reminders_ = snapshot;
    if (newAlert && !suppressed_) {                              // something rings: open on it, and keep it open until answered
        if (mascot_) mascot_->Notify();
        if (Present(Section::Reminders, true)) Expand();
        KillTimer(hwnd_, kAutoCloseTimer);
        autoClosePending_ = false;
    }
    if (wasRinging && reminders_.ringing.empty() && expanded_ && !Pinned() && !CursorInIsland(kLeaveSlack))
        SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);       // answered: close like any other time
    hover_ = -1;
    ApplyHeightTarget();
    Animate();
}

float IslandWindow::PermissionDetailH() const {
    if (permissions_.empty() || permissions_.front().id < kSoluApprovalBase) return 34.0f;
    const float width = kExpandedW - 24.0f - (mascot_ ? kMascotLane : 24.0f) - 20.0f;
    const float text = gfx_.MeasureHeight(permissions_.front().detail.c_str(), gfx_.Wrapped(12.5f, DWRITE_FONT_WEIGHT_NORMAL), width);
    return std::clamp(text + 10.0f, 34.0f, kMaxPanelH - 96.0f - 6.0f - 28.0f - 14.0f);
}

std::array<D2D1_RECT_F, 3> IslandWindow::PermissionButtons(const D2D1_RECT_F& island) const {
    const float x0 = island.left + 24;
    const float x1 = island.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    const float top = island.top + 96 + PermissionDetailH() + 6, bottom = top + 28;
    // A question has nothing to approve: one wide button takes the user to the window where it is waiting.
    if (!permissions_.empty() && permissions_.front().isQuestion) {
        const D2D1_RECT_F go = D2D1::RectF(x1 - 210, top, x1, bottom);
        const D2D1_RECT_F none = D2D1::RectF(-1, -1, -1, -1);     // off the window: never hit, never drawn
        return {none, go, none};
    }
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
    const bool solu = r.id >= kSoluApprovalBase;
    const D2D1_RECT_F box = D2D1::RectF(x0, base.top + 96, x1, base.top + 96 + PermissionDetailH());
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * alpha));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{box, 8, 8}, brush_.Get());
    if (r.risky) {
        brush_->SetColor(D2D1::ColorF(1.0f, 0.42f, 0.38f, 0.55f * alpha));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{box, 8, 8}, brush_.Get(), 1.0f);
    }
    const std::wstring detail = r.detail.empty() ? r.tool : r.detail;
    const bool wrapped = solu || r.isQuestion;                   // an email, or a question, reads better wrapped
    DrawText(dc, detail.c_str(), wrapped ? gfx_.Wrapped(12.5f, DWRITE_FONT_WEIGHT_NORMAL) : gfx_.Mono(11.0f),
             D2D1::RectF(box.left + 10, box.top + (wrapped ? 5.0f : 3.0f), box.right - 10, box.bottom - 3), WithAlpha(kText, 0.92f * alpha),
             DWRITE_TEXT_ALIGNMENT_LEADING, wrapped ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const auto buttons = PermissionButtons(base);
    IDWriteTextFormat* buttonFont = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (r.isQuestion) {                                          // one button: it goes to the window the question waits in
        const float ha = permissionHover_[1];
        brush_->SetColor(D2D1::ColorF(0.04f + 0.06f * ha, 0.47f + 0.06f * ha, 1.0f, alpha));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{buttons[1], 14, 14}, brush_.Get());
        const std::wstring label = L"Answer in " + r.source;
        DrawText(dc, label.c_str(), buttonFont, buttons[1], D2D1::ColorF(1, 1, 1, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    // Buttons: Reject, Approve, and a quiet way to answer in the tool itself instead.
    const float hr = permissionHover_[0], ha = permissionHover_[1], hd = permissionHover_[2];
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.10f + 0.06f * hr) * alpha));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{buttons[0], 14, 14}, brush_.Get());
    brush_->SetColor(D2D1::ColorF(0.04f + 0.06f * ha, 0.47f + 0.06f * ha, 1.0f, alpha));   // accent blue
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{buttons[1], 14, 14}, brush_.Get());
    DrawText(dc, L"Reject", buttonFont, buttons[0], WithAlpha(kText, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(dc, L"Approve", buttonFont, buttons[1], D2D1::ColorF(1, 1, 1, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
    const std::wstring deferLabel = solu ? (r.canAlways ? L"Always allow" : L"") : L"Answer in " + r.source;
    DrawText(dc, deferLabel.c_str(), smallFont, buttons[2], WithAlpha(kTextDim, (1.0f + 0.6f * hd) * alpha));
}

// ---- messages --------------------------------------------------------------------------------

float IslandWindow::TargetHeight() const {
    if (TourShown()) return kTourH;
    if (popover_ >= 0) return std::clamp(MenuHeight(popover_), kExpandedH, kMaxPanelH);
    if (section_ == Section::Ai && !permissions_.empty())         // a long approval card (an email) makes the island taller
        return std::clamp(96.0f + PermissionDetailH() + 6.0f + 28.0f + 14.0f, kExpandedH, kMaxPanelH);
    if (section_ == Section::Ai) return kAgentsH;                  // the agents panel
    if (section_ == Section::Shelf) return kShelfH;
    if (section_ == Section::Reminders) return reminders_.ringing.empty() ? kRemindersH : kAlarmH;
    if (section_ == Section::SafeShare) return kShareH;
    if (section_ == Section::Solu && SoluTaskRows() > 0 && !CaptionsShown()) return kSoluTasksH;
    if (CaptionsShown() && !solu_.said.empty()) {                // the live view grows so the whole answer fits
        const float width = kExpandedW - 22.0f - (mascot_ ? kMascotLane : 22.0f);
        float top = kTabTop + kTabH + 8.0f + 24.0f;
        if (!solu_.heard.empty()) top += solu_.lookImage ? 46.0f : 40.0f;
        const float text = gfx_.MeasureHeight(solu_.said.c_str(), gfx_.Wrapped(15.0f, DWRITE_FONT_WEIGHT_NORMAL), width);
        return std::clamp(top + text + 16.0f, kExpandedH, kMaxPanelH);
    }
    if (section_ == Section::Music) return kExpandedH + LyricShift();
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
                       base.bottom - 14.0f);
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
    const D2D1_RECT_F end = D2D1::RectF(r.right - 44, r.top - 3, r.right + 2, r.top + 19);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.06f + 0.14f * captionEndHover_) * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{end, 11, 11}, brush_.Get());
    DrawText(dc, L"End", gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), end, WithAlpha(kText, (0.55f + 0.45f * captionEndHover_) * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    const float right = r.right - 56;
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
    float heardLeft = r.left;
    if (solu_.lookImage && !solu_.lookImage->empty() && !solu_.heard.empty()) {              // what Solu looked at: always visible, so it is never a secret
        if (lookKey_ != solu_.lookImage.get() || !lookBitmap_) {
            lookKey_ = solu_.lookImage.get();
            lookBitmap_.Reset();
            const auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc_->CreateBitmap(D2D1::SizeU(solu_.lookImage->width, solu_.lookImage->height), solu_.lookImage->pixels.data(),
                              solu_.lookImage->width * 4, &props, &lookBitmap_);
        }
        if (lookBitmap_) {
            const float aspect = static_cast<float>(solu_.lookImage->width) / static_cast<float>(solu_.lookImage->height);
            float w = 64.0f, h = w / aspect;
            if (h > 40.0f) h = 40.0f, w = h * aspect;
            const D2D1_RECT_F thumb = D2D1::RectF(r.left, y, r.left + w, y + h);
            dc->DrawBitmap(lookBitmap_.Get(), thumb, 0.95f * a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            if (solu_.state == SoluState::Thinking) {                // a scanning line while it reads
                const float p = 0.5f + 0.5f * std::sin(now * 4.0f);
                brush_->SetColor(D2D1::ColorF(kAmber.r, kAmber.g, kAmber.b, 0.85f * a));
                dc->DrawLine(D2D1::Point2F(thumb.left, thumb.top + p * h), D2D1::Point2F(thumb.right, thumb.top + p * h), brush_.Get(), 1.5f);
            }
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.22f * a));
            dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{thumb, 3, 3}, brush_.Get(), 1.0f);
            heardLeft = thumb.right + 10;
            DrawText(dc, (L"Looking at " + solu_.lookTitle).c_str(), quiet, D2D1::RectF(heardLeft, y, r.right, y + 15), WithAlpha(kTextDim, 1.15f * a));
        }
    }
    if (!solu_.heard.empty()) {                                      // what you said
        const float top = heardLeft > r.left ? y + 15 : y;
        DrawText(dc, solu_.heard.c_str(), heardFont, D2D1::RectF(heardLeft, top, r.right, top + 38), WithAlpha(kTextDim, 1.3f * a),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        y += heardLeft > r.left ? 46 : 40;
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
    const bool voicePage = settingsPage_ == 1, mutePage = settingsPage_ == 2, islandPage = settingsPage_ == 3;
    const float headTop = area.top;

    // ---- header ----------------------------------------------------------------------------------------------
    if (voicePage || mutePage || islandPage) {                      // "< Voice" / "< Notifications": tap to go back
        const float hot = hover_ == kHitPopBack ? 1.0f : 0.0f;
        if (hot > 0) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.075f * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(area.left - 6, headTop - 2, area.left + 104, headTop + 26), 11, 11}, brush_.Get());
        }
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.78f + 0.2f * hot) * a));
        dc->DrawLine(D2D1::Point2F(area.left + 10, headTop + 7), D2D1::Point2F(area.left + 4, headTop + 13), brush_.Get(), 1.9f, roundStroke_.Get());
        dc->DrawLine(D2D1::Point2F(area.left + 4, headTop + 13), D2D1::Point2F(area.left + 10, headTop + 19), brush_.Get(), 1.9f, roundStroke_.Get());
        DrawText(dc, mutePage ? L"Notifications" : islandPage ? L"Closed island" : L"Voice", gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                 D2D1::RectF(area.left + 22, headTop, area.left + 160, headTop + 26),
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
        if (!voicePage && !mutePage && !islandPage) {              // the options page
            switch (k) {
            case 0: title = L"Solu AI"; note = soluNote; isSwitch = true; on = solu_.enabled; break;
            case 1: title = L"Solu's live captions"; note = L"Show your conversation with Solu on the island"; isSwitch = true; on = solu_.captions; break;
            case 2: title = L"Headphones mode"; note = solu_.headphones ? L"You can talk over Solu" : L"Solu waits until it has finished talking";
                    isSwitch = true; on = solu_.headphones; break;
            case 3: title = L"Screen help"; note = solu_.screen ? L"Say \u201cSolu, look at this\u201d \u00B7 only when you ask" : L"Solu will not look at your screen";
                    isSwitch = true; on = solu_.screen; break;
            case 4: title = L"Voice"; note = voiceName + L" · " + voiceNote; chevron = true; break;
            case 6:
                title = L"Closed island";
                note = glimpseLyrics_ && glimpseAgents_ ? L"Shows song lyrics and agents at work"
                       : glimpseLyrics_ ? L"Shows song lyrics · agents hidden"
                       : glimpseAgents_ ? L"Shows agents at work · lyrics hidden" : L"Lyrics and agents hidden";
                chevron = true;
                break;
            default: {
                title = L"Notifications";
                const size_t muted = mutes_.apps.size();
                note = mutes_.all ? L"All muted · they stay in the list" : muted == 0 ? L"Every app can open the island"
                       : muted == 1 ? mutes_.apps.front().name + L" is muted" : std::to_wstring(muted) + L" apps are muted";
                chevron = true;
                break;
            }
            }
        } else if (islandPage) {
            if (k == 0) {
                title = L"Song lyrics";
                note = glimpseLyrics_ ? L"The line being sung, while music plays" : L"Hidden (the Music tab still has them)";
                on = glimpseLyrics_;
            } else {
                title = L"Agent progress";
                note = glimpseAgents_ ? L"Claude Code and Codex at work, and when they finish" : L"Hidden (the Agents tab still has it)";
                on = glimpseAgents_;
            }
            isSwitch = true;
        } else if (mutePage) {
            if (k == 0) {
                title = L"Mute all";
                note = mutes_.all ? L"Nothing opens the island" : L"New notifications open the island";
                on = mutes_.all;
            } else {
                const MutedApp app = MuteRows()[static_cast<size_t>(k - 1)];
                title = app.name;
                on = mutes_.IsMuted(app.id, app.name);
                note = mutes_.all ? L"Muted with all" : on ? L"Muted · stays in the list" : L"Opens the island";
            }
            isSwitch = true;
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
    DrawText(dc, voicePage ? L"Solu says hello in the voice you pick" : mutePage ? L"Muted apps stay quiet on Windows and the island"
                 : islandPage ? L"What the thin bar may show when the island is closed" : L"Say “Solu” to talk · or press Ctrl+Alt+Space", quiet, g.footer,
             WithAlpha(kTextDim, 1.15f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---- the Solu AI tab -------------------------------------------------------------------------------------

int IslandWindow::SoluTaskRows() const {
    return std::min<int>(3, static_cast<int>(solu_.tasks.size()));
}

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
    case SoluState::Missing: title = L"Solu isn't set up yet"; sub = L"Tap to set it up"; tint = kAccent; break;
    case SoluState::Loading: title = L"Solu is waking up…"; sub = L"It loads its models, a few seconds."; tint = kAmber; break;
    case SoluState::Ready: title = L"Solu is listening"; sub = L"Say “Solu”, or press Ctrl+Alt+Space."; tint = kGreen; break;
    case SoluState::Listening: title = L"I'm listening"; sub = L"Go ahead."; tint = kGreen; break;
    case SoluState::Thinking: title = L"Thinking…"; sub = L""; tint = kAmber; break;
    case SoluState::Speaking: title = L"Solu"; sub = L""; tint = kGreen; break;
    }
    brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, alpha));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 4, y + 11), 4.0f, 4.0f), brush_.Get());
    DrawText(dc, title.c_str(), gfx_.Text(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left + 18, y, right, y + 22), WithAlpha(kText, alpha));
    const bool missingHot = solu_.state == SoluState::Missing && hover_ == kHitSoluSetup;
    DrawText(dc, sub.c_str(), gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(left + 18, y + 22, right, y + 40),
             solu_.state == SoluState::Missing ? WithAlpha(kAccent, (missingHot ? 1.0f : 0.85f) * alpha) : WithAlpha(kTextDim, alpha));
    y += 52.0f;

    if (const int rows = SoluTaskRows(); rows > 0) {                  // long tasks: what Solu is working on, with controls
        IDWriteTextFormat* name = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        IDWriteTextFormat* detail = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
        const float now = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;
        for (int i = 0; i < rows; ++i) {
            const SoluTask& t = solu_.tasks[static_cast<size_t>(i)];
            const float top = y + i * 40.0f;
            const bool live = t.status == L"running" || t.status == L"waiting" || t.status == L"planning" || t.status == L"paused";
            D2D1_COLOR_F c = t.status == L"done" ? kGreen : t.status == L"failed" ? kRed : t.status == L"waiting" ? Rgba(0.75f, 0.60f, 1.0f, 1.0f)
                           : t.status == L"paused" || t.status == L"cancelled" ? kNeutral : kAmber;
            const float pulse = t.status == L"running" || t.status == L"planning" ? 0.6f + 0.4f * std::sin(now * 5.0f) : 1.0f;
            brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, pulse * alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 4, top + 11), 3.6f, 3.6f), brush_.Get());
            std::wstring line;
            if (t.status == L"waiting") line = t.question;
            else if (t.status == L"paused") line = L"Paused \u00B7 step " + std::to_wstring(t.done + 1) + L" of " + std::to_wstring(t.total);
            else if (t.status == L"planning") line = L"Planning\u2026";
            else if (t.status == L"running") line = L"Step " + std::to_wstring(std::min(t.done + 1, t.total)) + L" of " + std::to_wstring(t.total) + L" \u00B7 " + t.step;
            else if (!t.result.empty()) line = t.result;
            else line = t.status == L"cancelled" ? L"Stopped" : t.status == L"failed" ? L"Couldn’t finish" : L"Done";
            DrawText(dc, t.title.c_str(), name, D2D1::RectF(left + 16, top, right - 68, top + 18), WithAlpha(kText, alpha));
            DrawText(dc, line.c_str(), detail, D2D1::RectF(left + 16, top + 18, right - 68, top + 36), WithAlpha(t.status == L"waiting" ? c : kTextDim, 1.15f * alpha));
            if (live && t.total > 0) {                                  // a thin progress line
                const float x0 = left + 16, x1 = right - 68;
                brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * alpha));
                dc->FillRectangle(D2D1::RectF(x0, top + 36, x1, top + 37.5f), brush_.Get());
                brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, 0.9f * alpha));
                dc->FillRectangle(D2D1::RectF(x0, top + 36, x0 + (x1 - x0) * static_cast<float>(t.done) / static_cast<float>(t.total), top + 37.5f), brush_.Get());
            }
            // buttons: pause / resume, and stop (or remove when it is over)
            auto button = [&](float cx, const wchar_t* glyph, int id) {
                const bool hot = hover_ == id;
                brush_->SetColor(D2D1::ColorF(1, 1, 1, (hot ? 0.18f : 0.07f) * alpha));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, top + 14), 12.0f, 12.0f), brush_.Get());
                DrawText(dc, glyph, gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(cx - 12, top + 2, cx + 12, top + 26),
                         WithAlpha(kText, (hot ? 1.0f : 0.8f) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
            };
            if (live) button(right - 47, t.status == L"paused" ? L"\uE768" : L"\uE769", kHitTaskPause + i);
            button(right - 15, live ? L"\uE71A" : L"\uE711", kHitTaskStop + i);
        }
        return;
    }
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

// ---- the file shelf -----------------------------------------------------------------------------

namespace {
bool HasFiles(IDataObject* data) {
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data && data->QueryGetData(&fmt) == S_OK;
}
} // namespace

// The left half of the island is the Shelf, the right half SafeShare (by side only, so it works while the island opens).
IslandWindow::DropZone IslandWindow::DropZoneAt(POINT screen) const {
    POINT cl = screen;
    ScreenToClient(hwnd_, &cl);
    const D2D1_RECT_F r = IslandRect();
    return ToDip(cl).x < (r.left + r.right) / 2 ? DropZone::Shelf : DropZone::SafeShare;
}

DWORD IslandWindow::FilesDragEnter(IDataObject* data, POINT screen) {
    if (shelfDragging_ || suppressed_ || !HasFiles(data)) return DROPEFFECT_NONE;      // not files, or our own drag coming back
    dragOver_ = true;
    dragOpened_ = !expanded_;
    KillTimer(hwnd_, kCloseTimer);
    KillTimer(hwnd_, kAutoCloseTimer);
    popover_ = -1;                                                    // (a menu would sit on top of the zones)
    Expand();
    return FilesDragOver(screen);
}

DWORD IslandWindow::FilesDragOver(POINT screen) {
    if (!dragOver_) return DROPEFFECT_NONE;
    KillTimer(hwnd_, kCloseTimer);
    const DropZone zone = DropZoneAt(screen);
    if (zone != dropZone_) {
        dropZone_ = zone;
        Animate();
    }
    return zone == DropZone::SafeShare && !ShareTakesDrops() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
}

void IslandWindow::FilesDragLeave() {
    if (!dragOver_) return;
    dragOver_ = false;
    if (dragOpened_ && expanded_) SetTimer(hwnd_, kCloseTimer, 700, nullptr);             // it opened for the drag: close again
    Animate();
}

DWORD IslandWindow::FilesDrop(IDataObject* data, POINT screen) {
    dragOver_ = false;
    const DropZone zone = DropZoneAt(screen);
    std::vector<std::wstring> paths;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (data && data->GetData(&fmt, &medium) == S_OK) {
        if (HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH * 4];
                if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) paths.emplace_back(path);
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    if (zone == DropZone::SafeShare) {                                // to send: the SafeShare tab, open, to fill in
        if (!ShareTakesDrops() || paths.empty()) {
            Animate();
            return DROPEFFECT_NONE;
        }
        OnClick(static_cast<int>(Section::SafeShare));
        shareSentView_ = false;
        AddShareFiles(paths);
        autoClosePending_ = false;                                    // it stays open: the email is still to be written
        Animate();
        return DROPEFFECT_COPY;
    }
    if (section_ != Section::Shelf) OnClick(static_cast<int>(Section::Shelf));   // to hold: the Shelf, briefly, then it closes
    if (!paths.empty() && shelfAdd_) shelfAdd_(paths);
    SetTimer(hwnd_, kAutoCloseTimer, 3200, nullptr);                  // it stays a moment, so you can see it landed
    autoClosePending_ = dragOpened_;
    Animate();
    return paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
}

void IslandWindow::SetShelf(const ShelfSnapshot& snapshot) {
    const bool first = shelfSeen_.empty() && shelf_.items.empty();
    shelf_ = snapshot;
    std::set<std::wstring> now;
    for (const ShelfItem& i : shelf_.items) {
        now.insert(i.path);
        if (!shelfSeen_.count(i.path)) shelfPop_[i.path] = first ? 1.0f : 0.0f;       // new ones pop in
    }
    shelfSeen_ = now;
    for (auto it = shelfSel_.begin(); it != shelfSel_.end();)
        it = now.count(*it) ? std::next(it) : shelfSel_.erase(it);
    for (auto it = shelfBitmaps_.begin(); it != shelfBitmaps_.end();)
        it = now.count(it->first) ? std::next(it) : shelfBitmaps_.erase(it);
    for (auto it = shelfPop_.begin(); it != shelfPop_.end();)
        it = now.count(it->first) ? std::next(it) : shelfPop_.erase(it);
    shelfHover_.resize(shelf_.items.size(), 0.0f);
    if (expanded_ && section_ == Section::Shelf) Animate();
}

D2D1_RECT_F IslandWindow::ShelfArea(const D2D1_RECT_F& island) const {
    return D2D1::RectF(island.left + 22, island.top + 84, island.left + kExpandedW - kMascotLane + 6, island.top + 84 + kCardH + 4);
}

D2D1_RECT_F IslandWindow::ShelfCard(const D2D1_RECT_F& island, int index) const {
    const D2D1_RECT_F area = ShelfArea(island);
    const float x = area.left - shelfScroll_ + static_cast<float>(index) * (kCardW + kCardGap);
    return D2D1::RectF(x, area.top, x + kCardW, area.top + kCardH);
}

ID2D1Bitmap1* IslandWindow::Thumb(const ImageBGRA& image) {
    if (auto it = thumbs_.find(&image); it != thumbs_.end()) return it->second.Get();
    Com<ID2D1Bitmap1> bitmap;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(image.width, image.height), image.pixels.data(), image.width * 4, &props, &bitmap))) return nullptr;
    return thumbs_.emplace(&image, std::move(bitmap)).first->second.Get();
}

ID2D1Bitmap1* IslandWindow::ShelfBitmap(const ShelfItem& item) {
    auto it = shelfBitmaps_.find(item.path);
    if (it != shelfBitmaps_.end()) return it->second.Get();
    if (!item.icon || item.icon->empty() || item.icon->width < 2) return nullptr;
    Com<ID2D1Bitmap1> bitmap;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(item.icon->width, item.icon->height), item.icon->pixels.data(), item.icon->width * 4, &props, &bitmap))) return nullptr;
    return shelfBitmaps_.emplace(item.path, std::move(bitmap)).first->second.Get();
}

void IslandWindow::DrawShelf(ID2D1DeviceContext* dc, D2D1_RECT_F base, float a) {
    const D2D1_RECT_F area = ShelfArea(base);
    const int n = static_cast<int>(shelf_.items.size());
    const float now = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;
    IDWriteTextFormat* head = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* dim = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL);

    // ---- header
    std::wstring title = n == 0 ? L"Your shelf" : n == 1 ? L"1 item held" : std::to_wstring(n) + L" items held";
    DrawText(dc, title.c_str(), head, D2D1::RectF(area.left, base.top + 56, area.left + 220, base.top + 78), WithAlpha(kText, a));
    if (n > 0) {
        DrawText(dc, L"Drag them out into any folder, app or web page", dim, D2D1::RectF(area.left + 110, base.top + 57, area.right - 56, base.top + 78),
                 WithAlpha(kTextDim, 1.05f * a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        const bool hot = hover_ == kHitShelfClear;
        DrawText(dc, L"Clear", gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(area.right - 50, base.top + 56, area.right, base.top + 78),
                 WithAlpha(hot ? kAccent : kTextDim, (hot ? 1.0f : 1.2f) * a), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // ---- the cards
    dc->PushAxisAlignedClip(D2D1::RectF(area.left - 4, area.top - 6, area.right + 4, area.bottom + 6), D2D1_ANTIALIAS_MODE_ALIASED);
    if (n == 0) {                                                       // empty: one big drop zone
        const D2D1_RECT_F zone = D2D1::RectF(area.left, area.top, area.right, area.bottom - 4);
        const bool hot = hover_ == kHitShelfAdd;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (hot ? 0.10f : 0.055f) * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{zone, 16, 16}, brush_.Get());
        const D2D1_POINT_2F c = D2D1::Point2F((zone.left + zone.right) / 2, zone.top + 34);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.55f * a));
        dc->DrawLine(D2D1::Point2F(c.x - 9, c.y), D2D1::Point2F(c.x + 9, c.y), brush_.Get(), 2.2f, roundStroke_.Get());
        dc->DrawLine(D2D1::Point2F(c.x, c.y - 9), D2D1::Point2F(c.x, c.y + 9), brush_.Get(), 2.2f, roundStroke_.Get());
        DrawText(dc, L"Drop files here to hold them", gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                 D2D1::RectF(zone.left, zone.top + 52, zone.right, zone.top + 72), WithAlpha(kText, 0.95f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"then drag them to any folder, app or website  ·  or click to choose", dim,
                 D2D1::RectF(zone.left, zone.top + 72, zone.right, zone.top + 90), WithAlpha(kTextDim, 1.1f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    for (int i = 0; i < n; ++i) {
        const ShelfItem& item = shelf_.items[static_cast<size_t>(i)];
        D2D1_RECT_F c = ShelfCard(base, i);
        if (c.right < area.left - 4 || c.left > area.right + 4) continue;
        const float pop = shelfPop_.count(item.path) ? shelfPop_[item.path] : 1.0f;
        const float hv = i < static_cast<int>(shelfHover_.size()) ? shelfHover_[static_cast<size_t>(i)] : 0.0f;
        const bool selected = shelfSel_.count(item.path) != 0;
        const float grow = (0.82f + 0.18f * pop) * (1.0f + 0.03f * hv);           // pops in, lifts on hover
        const float cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2 - 2.0f * hv;
        const float hw = kCardW / 2 * grow, hh = kCardH / 2 * grow;
        c = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
        const float alpha = a * std::min(1.0f, pop * 1.4f);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (selected ? 0.16f : 0.065f + 0.075f * hv) * alpha));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{c, 14, 14}, brush_.Get());
        if (selected) {
            brush_->SetColor(D2D1::ColorF(kAccent.r, kAccent.g, kAccent.b, 0.9f * alpha));
            dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(c.left + 0.75f, c.top + 0.75f, c.right - 0.75f, c.bottom - 0.75f), 14, 14}, brush_.Get(), 1.5f);
        }
        // picture: a thumbnail or the file's icon, fitted into a 54 px box
        const D2D1_RECT_F box = D2D1::RectF(cx - 27 * grow, c.top + 9 * grow, cx + 27 * grow, c.top + 63 * grow);
        if (ID2D1Bitmap1* bmp = ShelfBitmap(item)) {
            const D2D1_SIZE_F sz = bmp->GetSize();
            const float fit = std::min((box.right - box.left) / sz.width, (box.bottom - box.top) / sz.height);
            const float w = sz.width * fit, h = sz.height * fit, bx = (box.left + box.right) / 2, by = (box.top + box.bottom) / 2;
            dc->DrawBitmap(bmp, D2D1::RectF(bx - w / 2, by - h / 2, bx + w / 2, by + h / 2), (item.missing ? 0.35f : 1.0f) * alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else {                                                          // not loaded yet: a soft placeholder
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.08f + 0.05f * std::sin(now * 5.0f + i)) * alpha));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(box.left + 6, box.top + 6, box.right - 6, box.bottom - 6), 8, 8}, brush_.Get());
        }
        DrawText(dc, item.name.c_str(), gfx_.Wrapped(10.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(c.left + 5, c.top + 66 * grow, c.right - 5, c.bottom - 3),
                 WithAlpha(kText, (item.missing ? 0.45f : 0.92f) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (hv > 0.02f) {                                                  // x: take it off the shelf (the file itself is untouched)
            const D2D1_POINT_2F xc = D2D1::Point2F(c.right - 11, c.top + 11);
            const bool hot = hover_ == kHitShelfRemove + i;
            brush_->SetColor(D2D1::ColorF(hot ? 0.95f : 0.18f, hot ? 0.35f : 0.18f, hot ? 0.33f : 0.20f, 0.96f * hv * alpha));
            dc->FillEllipse(D2D1::Ellipse(xc, 8.0f, 8.0f), brush_.Get());
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * hv * alpha));
            dc->DrawLine(D2D1::Point2F(xc.x - 2.7f, xc.y - 2.7f), D2D1::Point2F(xc.x + 2.7f, xc.y + 2.7f), brush_.Get(), 1.4f, roundStroke_.Get());
            dc->DrawLine(D2D1::Point2F(xc.x + 2.7f, xc.y - 2.7f), D2D1::Point2F(xc.x - 2.7f, xc.y + 2.7f), brush_.Get(), 1.4f, roundStroke_.Get());
        }
    }
    if (n > 0 && n < 60) {                                                // the "add more" card
        const D2D1_RECT_F c = ShelfCard(base, n);
        const bool hot = hover_ == kHitShelfAdd;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (hot ? 0.10f : 0.045f) * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{c, 14, 14}, brush_.Get());
        const D2D1_POINT_2F m = D2D1::Point2F((c.left + c.right) / 2, (c.top + c.bottom) / 2);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (hot ? 0.9f : 0.5f) * a));
        dc->DrawLine(D2D1::Point2F(m.x - 8, m.y), D2D1::Point2F(m.x + 8, m.y), brush_.Get(), 2.0f, roundStroke_.Get());
        dc->DrawLine(D2D1::Point2F(m.x, m.y - 8), D2D1::Point2F(m.x, m.y + 8), brush_.Get(), 2.0f, roundStroke_.Get());
    }
    dc->PopAxisAlignedClip();
}

// Files hovering over the island: two zones side by side, the Shelf (hold them, drag them out later) and SafeShare (send
// them). The one under the files lights up and breathes; the cat has stepped aside (see Render) to make room.
void IslandWindow::DrawDropChooser(ID2D1DeviceContext* dc, const D2D1_RECT_F& island, float alpha) {
    const float t = dragT_ * alpha;
    if (t < 0.01f) return;
    const float now = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;
    const D2D1_RECT_F area = D2D1::RectF(island.left + 16, island.top + kTabTop + kTabH + 8, island.right - 16, island.bottom - 14);
    if (area.bottom - area.top < 40) return;                          // (still opening)
    brush_->SetColor(D2D1::ColorF(kIslandFill.r, kIslandFill.g, kIslandFill.b, t));          // opaque: the tab below must not show through
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(area.left - 6, area.top - 6, area.right + 6, area.bottom + 6), 20, 20}, brush_.Get());

    Com<ID2D1StrokeStyle> dashed;
    const D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
    gfx_.d2dFactory()->CreateStrokeStyle(sp, nullptr, 0, &dashed);
    const float mid = (area.left + area.right) / 2;
    struct Zone {
        D2D1_RECT_F box;
        float hot;                                                    // 0..1: the files are over this one
        bool open;                                                    // it takes the files now
        const wchar_t *glyph, *title, *line;
    };
    const bool shareOpen = ShareTakesDrops();
    const Zone zones[2] = {
        {D2D1::RectF(area.left, area.top, mid - 6, area.bottom), 1.0f - dropShare_, true, L"", L"Shelf", L"Hold it here, drag it out later"},
        {D2D1::RectF(mid + 6, area.top, area.right, area.bottom), dropShare_, shareOpen, L"", L"SafeShare",
         shareOpen ? L"Send it by email or to your phone" : L"A send is on its way. Wait for it to finish"},
    };
    for (const Zone& z : zones) {
        const float hot = z.open ? z.hot : 0.0f;
        const D2D1_COLOR_F tint = z.open ? kAccent : kTextDim;
        brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, (0.05f + (0.14f + 0.05f * std::sin(now * 6.0f)) * hot) * t));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{z.box, 16, 16}, brush_.Get());
        brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, (0.30f + 0.65f * hot) * t));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(z.box.left + 1, z.box.top + 1, z.box.right - 1, z.box.bottom - 1), 15, 15},
                                 brush_.Get(), 1.0f + 0.8f * hot, dashed.Get());
        const float cy = (z.box.top + z.box.bottom) / 2;
        const D2D1_POINT_2F c = D2D1::Point2F((z.box.left + z.box.right) / 2, cy - 20);
        const float r = 18.0f * (1.0f + 0.06f * hot * std::sin(now * 6.0f));
        if (z.open) brush_->SetColor(D2D1::ColorF(0.04f, 0.47f, 1.0f, (0.35f + 0.65f * hot) * t));
        else brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * t));
        dc->FillEllipse(D2D1::Ellipse(c, r, r), brush_.Get());
        DrawText(dc, z.glyph, gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r),
                 D2D1::ColorF(1, 1, 1, (z.open ? 1.0f : 0.5f) * t), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, z.title, gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(z.box.left + 8, cy + 4, z.box.right - 8, cy + 24),
                 WithAlpha(kText, (z.open ? 0.75f + 0.25f * hot : 0.5f) * t), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, z.line, gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(z.box.left + 8, cy + 24, z.box.right - 8, cy + 42),
                 WithAlpha(kTextDim, 1.15f * t), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void IslandWindow::OpenShelfItem(int index, bool reveal) {
    if (index < 0 || index >= static_cast<int>(shelf_.items.size())) return;
    const std::wstring& path = shelf_.items[static_cast<size_t>(index)].path;
    if (reveal) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void IslandWindow::ShelfAddFiles() {
    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Add files to the shelf";
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> paths;
    const std::wstring first = buffer.data();
    const wchar_t* next = buffer.data() + first.size() + 1;
    if (*next == L'\0') {
        paths.push_back(first);                                           // a single file: the full path
    } else {
        for (; *next; next += wcslen(next) + 1) paths.push_back(first + L"\\" + next);
    }
    if (shelfAdd_) shelfAdd_(paths);
}

// Pull one or several items off the shelf into another app, folder or web page: a real shell drag of the files.
void IslandWindow::StartShelfDrag(int index) {
    if (index < 0 || index >= static_cast<int>(shelf_.items.size())) return;
    const std::wstring& pressed = shelf_.items[static_cast<size_t>(index)].path;
    if (!shelfSel_.count(pressed)) shelfSel_ = {pressed};               // grabbing an unselected item drags just that one
    std::vector<std::wstring> paths;
    for (const ShelfItem& i : shelf_.items)
        if (shelfSel_.count(i.path) && !i.missing) paths.push_back(i.path);
    if (paths.empty()) return;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    std::vector<PCUITEMID_CHILD> items;                                // absolute PIDLs: no parent folder is given
    for (const std::wstring& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            pidls.push_back(pidl);
            items.push_back(reinterpret_cast<PCUITEMID_CHILD>(pidl));
        }
    }
    IDataObject* data = nullptr;
    std::vector<PCIDLIST_ABSOLUTE> constPidls(pidls.begin(), pidls.end());
    if (!pidls.empty()) {                                               // a shell item array makes the full set of file formats (file list, IDList, names)
        IShellItemArray* array = nullptr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(constPidls.size()), constPidls.data(), &array)) && array) {
            array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
            array->Release();
        }
    }
    if (data) {
        pressed_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        shelfDragging_ = true;                                          // the island stays open while the drag lasts
        KillTimer(hwnd_, kCloseTimer);
        DWORD effect = DROPEFFECT_NONE;
        SHDoDragDrop(hwnd_, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        shelfDragging_ = false;
        data->Release();
        Log("shelf: dragged %zu item(s) out (effect %lu)", paths.size(), static_cast<unsigned long>(effect));
    }
    for (PIDLIST_ABSOLUTE pidl : pidls) CoTaskMemFree(pidl);
    hover_ = -1;
    if (expanded_ && !Pinned() && !CursorInIsland(kLeaveSlack)) SetTimer(hwnd_, kCloseTimer, 900, nullptr);
    Animate();
}

// ---- the Agents tab ----------------------------------------------------------------------------

namespace {
std::wstring AgoText(ULONGLONG unixMs) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const ULONGLONG now = ((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000 - 11644473600000ull;
    const ULONGLONG sec = now > unixMs ? (now - unixMs) / 1000 : 0;
    if (sec < 20) return L"just now";
    if (sec < 90) return L"a minute ago";
    if (sec < 3600) return std::to_wstring(sec / 60) + L" min ago";
    if (sec < 86400) return std::to_wstring(sec / 3600) + L" h ago";
    return std::to_wstring(sec / 86400) + L" d ago";
}
} // namespace

std::vector<const AgentSession*> IslandWindow::SessionsOf(AgentTool tool) const {
    std::vector<const AgentSession*> out;
    for (const AgentSession& s : agents_.sessions)
        if (s.tool == tool) out.push_back(&s);
    return out;
}

const AgentSession* IslandWindow::CurrentSession() const {
    const auto mine = SessionsOf(agentTool_);
    if (mine.empty()) return nullptr;
    for (const AgentSession* s : mine)
        if (!agentSession_.empty() && s->id == agentSession_) return s;
    return mine.front();
}

void IslandWindow::AutoPickAgent() {
    if (!agentAuto_ || agents_.sessions.empty()) return;
    const AgentSession& best = agents_.sessions.front();                  // working first, then the most recent
    if (best.tool != agentTool_) agentSession_.clear();
    agentTool_ = best.tool;
}

void IslandWindow::SetAgents(const AgentSnapshot& snapshot) {
    agents_ = snapshot;
    if (!snapshot.justStarted.empty()) {                                // a prompt from here started a new session: go to it
        for (const AgentSession& s : agents_.sessions)
            if (s.id == snapshot.justStarted) {
                agentTool_ = s.tool;
                agentSession_ = s.id;
                agentScroll_ = 0;
                newChat_ = false;
            }
    } else {
        AutoPickAgent();
    }
    if (!snapshot.sendError.empty()) agentError_ = snapshot.sendError;
    UpdateAgentTimer();
    if (expanded_ && section_ == Section::Ai) Animate();
}

void IslandWindow::UpdateAgentTimer() {
    KillTimer(hwnd_, kAgentTimer);
    if (!expanded_ || section_ != Section::Ai) return;
    bool busy = agents_.sending || field_;
    for (const AgentSession& s : agents_.sessions)
        if (s.status == AgentStatus::Working) busy = true;
    if (busy) SetTimer(hwnd_, kAgentTimer, field_ ? 260 : 120, nullptr);
}

IslandWindow::AgentsLayout IslandWindow::AgentsGeometry(const D2D1_RECT_F& island) const {
    AgentsLayout g{};
    const float x0 = island.left + 22, x1 = island.left + kExpandedW - kMascotLane + 6, top = island.top;
    const float bottom = island.bottom - 14;
    g.tool[0] = D2D1::RectF(x0, top + 56, x0 + 152, top + 90);
    g.tool[1] = D2D1::RectF(x0 + 158, top + 56, x0 + 310, top + 90);
    g.connect = D2D1::RectF(x0 + 316, top + 56, x1, top + 90);
    g.header = D2D1::RectF(x0, top + 98, x1 - 50, top + 120);
    g.prev = D2D1::RectF(x1 - 46, top + 96, x1 - 24, top + 122);
    g.next = D2D1::RectF(x1 - 22, top + 96, x1, top + 122);
    g.send = D2D1::RectF(x1 - 32, bottom - 32, x1, bottom);
    g.box = D2D1::RectF(x0 + 38, bottom - 32, x1 - 38, bottom);
    g.newBtn = D2D1::RectF(x0, bottom - 32, x0 + 32, bottom);
    g.feed = D2D1::RectF(x0, top + 126, x1, bottom - 40);
    g.rows = std::max(1, static_cast<int>((g.feed.bottom - g.feed.top) / 21.0f));
    return g;
}

ID2D1Bitmap1* IslandWindow::Logo(AgentTool tool) {
    Com<ID2D1Bitmap1>& slot = tool == AgentTool::Claude ? logoClaude_ : logoCodex_;
    if (slot) return slot.Get();
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring path(exe, n);
    path = path.substr(0, path.find_last_of(L'\\')) + (tool == AgentTool::Claude ? L"\\assets\\agents\\claude.png" : L"\\assets\\agents\\codex.png");
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    std::vector<uint8_t> bytes(GetFileSize(f, nullptr));
    DWORD got = 0;
    ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
    CloseHandle(f);
    ImageBGRA img;
    if (!DecodeImage(bytes.data(), got, 1.0f, 128, img)) return nullptr;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc_->CreateBitmap(D2D1::SizeU(img.width, img.height), img.pixels.data(), img.width * 4, &props, &slot);
    return slot.Get();
}

void IslandWindow::DrawAgents(ID2D1DeviceContext* dc, D2D1_RECT_F base, float a) {
    const AgentsLayout g = AgentsGeometry(base);
    const uint64_t now = GetTickCount64();
    const AgentSession* cur = CurrentSession();
    IDWriteTextFormat* name = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* caption = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* body = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* bold = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const D2D1_COLOR_F kViolet = Rgba(0.75f, 0.60f, 1.0f, 1.0f);

    // ---- the two agents: logo, name, and what each is doing
    const wchar_t* names[2] = {L"Claude Code", L"Codex"};
    for (int i = 0; i < 2; ++i) {
        const AgentTool tool = i == 0 ? AgentTool::Claude : AgentTool::Codex;
        const bool selected = agentTool_ == tool;
        const bool hot = hover_ == kHitAgentTool + i;
        const D2D1_RECT_F r = g.tool[i];
        if (selected || hot) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (selected ? 0.12f : 0.07f) * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{r, 17, 17}, brush_.Get());
        }
        const D2D1_RECT_F logo = D2D1::RectF(r.left + 8, (r.top + r.bottom) / 2 - 11, r.left + 30, (r.top + r.bottom) / 2 + 11);
        if (ID2D1Bitmap1* bmp = Logo(tool)) dc->DrawBitmap(bmp, logo, (selected ? 1.0f : 0.72f) * a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        DrawText(dc, names[i], name, D2D1::RectF(logo.right + 8, r.top, r.right - 24, r.bottom), WithAlpha(kText, (selected ? 1.0f : 0.72f) * a));
        // a dot: amber and pulsing while it works, green when idle recently, grey when nothing was seen
        const auto mine = SessionsOf(tool);
        bool working = false;
        for (const AgentSession* s : mine) working |= s->status == AgentStatus::Working;
        const D2D1_COLOR_F dot = working ? kAmber : mine.empty() ? Rgba(1, 1, 1, 0.28f) : kGreen;
        const float pulse = working ? 0.55f + 0.45f * std::sin(static_cast<float>(now % 1200) / 1200.0f * 6.2831853f) : 1.0f;
        brush_->SetColor(D2D1::ColorF(dot.r, dot.g, dot.b, dot.a * pulse * a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.right - 14, (r.top + r.bottom) / 2), 3.5f, 3.5f), brush_.Get());
    }
    const bool hooked = agentTool_ == AgentTool::Claude ? agents_.claudeHooked : agents_.codexHooked;
    if (!hooked) {                                                        // approvals from your editor need the hook (prompts from here never do)
        const bool hot = hover_ == kHitConnect;
        DrawText(dc, L"Connect approvals", caption, g.connect, WithAlpha(hot ? kText : kTextDim, 1.1f * a), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // ---- the session: project, title, and its state
    const auto mine = SessionsOf(agentTool_);
    if (!cur) {
        const bool found = agentTool_ == AgentTool::Claude ? agents_.claudeFound : agents_.codexFound;
        DrawText(dc, found ? L"Nothing running right now" : L"Not installed on this PC", gfx_.Text(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                 D2D1::RectF(g.header.left, g.feed.top + 6, g.send.right, g.feed.top + 30), WithAlpha(kText, 0.95f * a));
        DrawText(dc, found ? L"Send a prompt below to start a new chat" : L"Install its VS Code extension to send prompts from here", caption,
                 D2D1::RectF(g.header.left, g.feed.top + 32, g.send.right, g.feed.top + 52), WithAlpha(kTextDim, 1.1f * a));
    } else {
        D2D1_COLOR_F tint = cur->status == AgentStatus::Working ? kAmber : cur->status == AgentStatus::Waiting ? kViolet : kGreen;
        brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(g.header.left + 4, g.header.top + 11), 3.2f, 3.2f), brush_.Get());
        const std::wstring state = cur->status == AgentStatus::Working ? cur->doing
                                 : cur->status == AgentStatus::Waiting ? L"Waiting for you" : L"Done · " + AgoText(cur->lastActivity);
        const float stateW = std::min(std::ceil(gfx_.MeasureWidth(state.c_str(), caption)) + 4, 200.0f);
        const float nameW = std::ceil(gfx_.MeasureWidth(cur->project.c_str(), bold));
        const bool go = hover_ == kHitAgentGo;
        const float titleLeft = g.header.left + 14 + nameW + 24;
        DrawText(dc, cur->project.c_str(), bold, D2D1::RectF(g.header.left + 14, g.header.top, g.header.left + 14 + nameW + 2, g.header.bottom),
                 go ? WithAlpha(kAccent, a) : WithAlpha(kText, a));
        DrawText(dc, L"\uE8A7", gfx_.Text(9.5f, DWRITE_FONT_WEIGHT_NORMAL, true),                 // "open in a new window"
                 D2D1::RectF(g.header.left + 16 + nameW, g.header.top + 1, g.header.left + 30 + nameW, g.header.bottom),
                 go ? WithAlpha(kAccent, a) : WithAlpha(kTextDim, 1.1f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (go) {
            brush_->SetColor(D2D1::ColorF(kAccent.r, kAccent.g, kAccent.b, 0.7f * a));
            dc->DrawLine(D2D1::Point2F(g.header.left + 14, g.header.bottom - 1.5f), D2D1::Point2F(g.header.left + 14 + nameW, g.header.bottom - 1.5f), brush_.Get(), 1.0f);
        }
        DrawText(dc, cur->title.c_str(), caption, D2D1::RectF(titleLeft, g.header.top + 1, g.header.right - stateW - 8, g.header.bottom),
                 WithAlpha(kTextDim, 1.15f * a));
        DrawText(dc, state.c_str(), caption, D2D1::RectF(g.header.right - stateW, g.header.top + 1, g.header.right + (mine.size() > 1 ? 0.0f : 48.0f), g.header.bottom),
                 WithAlpha(cur->status == AgentStatus::Working ? tint : kTextDim, 1.15f * a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (mine.size() > 1) {                                            // flip between this agent's sessions
            for (int k = 0; k < 2; ++k) {
                const D2D1_RECT_F r = k == 0 ? g.prev : g.next;
                const bool hot = hover_ == (k == 0 ? kHitAgentPrev : kHitAgentNext);
                const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, d = k == 0 ? -1.0f : 1.0f;
                brush_->SetColor(D2D1::ColorF(1, 1, 1, (hot ? 0.95f : 0.55f) * a));
                dc->DrawLine(D2D1::Point2F(cx - 2.5f * d, cy - 4.5f), D2D1::Point2F(cx + 2.5f * d, cy), brush_.Get(), 1.7f, roundStroke_.Get());
                dc->DrawLine(D2D1::Point2F(cx + 2.5f * d, cy), D2D1::Point2F(cx - 2.5f * d, cy + 4.5f), brush_.Get(), 1.7f, roundStroke_.Get());
            }
        }

        // ---- the feed: the last things it said and did, newest at the bottom. Replies wrap onto several lines (the final
        //      answer gets the most), everything else is one line.
        dc->PushAxisAlignedClip(g.feed, D2D1_ANTIALIAS_MODE_ALIASED);
        const int total = static_cast<int>(cur->feed.size());
        const int last = total - agentScroll_;                           // one past the newest row shown
        int finalReply = -1;                                              // the agent's last message: the answer
        for (int k = total - 1; k >= 0; --k)
            if (cur->feed[static_cast<size_t>(k)].kind == AgentEvent::Kind::Said) { finalReply = k; break; }
        const float textW = g.feed.right - g.feed.left - 22;
        auto rowHeight = [&](int k, int& lines) {
            const AgentEvent& e = cur->feed[static_cast<size_t>(k)];
            lines = 1;
            if (e.kind == AgentEvent::Kind::Said || e.kind == AgentEvent::Kind::You) {
                const int cap = k == finalReply ? 6 : 2;
                const float h = gfx_.MeasureHeight(e.text.c_str(), gfx_.Wrapped(12.5f, DWRITE_FONT_WEIGHT_NORMAL), textW);
                lines = std::clamp(static_cast<int>(std::ceil(h / 16.5f)), 1, cap);
            }
            return std::max(21.0f, lines * 16.5f + 5.0f);
        };
        struct Row { int k; float h; int lines; };
        std::vector<Row> rows;
        float used = 0;
        const float avail = g.feed.bottom - g.feed.top;
        for (int k = last - 1; k >= 0; --k) {
            int lines;
            float h = rowHeight(k, lines);
            if (!rows.empty() && used + h > avail) {                       // too tall: show as much of it as fits, rather than nothing
                if (avail - used >= 38) rows.push_back({k, avail - used, lines}), used = avail;
                break;
            }
            rows.push_back({k, h, lines});
            used += h;
        }
        float y = g.feed.bottom - used;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            const AgentEvent& e = cur->feed[static_cast<size_t>(it->k)];
            const float rowH = it->h;
            const D2D1_RECT_F row = D2D1::RectF(g.feed.left, y, g.feed.right, y + rowH);
            const float tx = row.left + 22;
            const D2D1_RECT_F textRect = D2D1::RectF(tx, y + 2, row.right, y + rowH);
            if (e.kind == AgentEvent::Kind::You) {
                DrawText(dc, L"", gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(row.left, y, row.left + 18, y + 21),
                         WithAlpha(kAccent, a), DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawText(dc, e.text.c_str(), gfx_.Wrapped(12.5f, DWRITE_FONT_WEIGHT_NORMAL), textRect, WithAlpha(kAccent, 1.0f * a),
                         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            } else if (e.kind == AgentEvent::Kind::Said) {
                const bool answer = it->k == finalReply;
                brush_->SetColor(D2D1::ColorF(1, 1, 1, (answer ? 0.8f : 0.45f) * a));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(row.left + 9, y + 11), answer ? 2.8f : 2.2f, answer ? 2.8f : 2.2f), brush_.Get());
                DrawText(dc, e.text.c_str(), gfx_.Wrapped(12.5f, DWRITE_FONT_WEIGHT_NORMAL), textRect, WithAlpha(kText, (answer ? 0.98f : 0.8f) * a),
                         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            } else if (e.kind == AgentEvent::Kind::Tool) {
                if (e.running) {                                          // three dots chasing each other
                    for (int d = 0; d < 3; ++d) {
                        const float ph = 0.5f + 0.5f * std::sin(static_cast<float>(now % 100000) / 1000.0f * 7.0f - d * 0.9f);
                        brush_->SetColor(D2D1::ColorF(kAmber.r, kAmber.g, kAmber.b, (0.3f + 0.7f * ph) * a));
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(row.left + 3 + d * 5.5f, y + 11), 1.9f, 1.9f), brush_.Get());
                    }
                } else {
                    DrawText(dc, L"", gfx_.Text(10.0f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(row.left, y, row.left + 18, y + 21),
                             WithAlpha(kTextDim, 1.2f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                const float verbW = std::ceil(gfx_.MeasureWidth(e.text.c_str(), bold));
                DrawText(dc, e.text.c_str(), bold, D2D1::RectF(tx, y + 1, tx + verbW + 2, y + 21), WithAlpha(e.running ? kAmber : kText, (e.running ? 1.0f : 0.8f) * a));
                DrawText(dc, e.detail.c_str(), body, D2D1::RectF(tx + verbW + 8, y + 1, row.right, y + 21), WithAlpha(kTextDim, 1.2f * a));
            } else if (e.kind == AgentEvent::Kind::Done) {
                DrawText(dc, L"", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(row.left, y, row.left + 18, y + 21),
                         WithAlpha(kGreen, a), DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawText(dc, e.text.c_str(), bold, D2D1::RectF(tx, y + 1, row.right, y + 21), WithAlpha(kGreen, a));
            } else {
                DrawText(dc, L"", gfx_.Text(10.0f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(row.left, y, row.left + 18, y + 21),
                         WithAlpha(D2D1::ColorF(1.0f, 0.45f, 0.40f, 1.0f), a), DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawText(dc, e.text.c_str(), body, D2D1::RectF(tx, y + 1, row.right, y + 21), WithAlpha(D2D1::ColorF(1.0f, 0.55f, 0.50f, 1.0f), a));
            }
            y += rowH;
        }
        dc->PopAxisAlignedClip();
        if (agentScroll_ > 0)                                             // there is newer stuff below
            DrawText(dc, L"↓ newer", caption, D2D1::RectF(g.feed.right - 70, g.feed.bottom - 18, g.feed.right, g.feed.bottom), WithAlpha(kTextDim, 1.2f * a),
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // ---- the prompt box: type like you would to the agent in your editor
    const bool sendingHere = agents_.sending && agents_.sendingTool == agentTool_;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (newChat_ ? 0.20f : hover_ == kHitNewChat ? 0.14f : 0.08f) * a));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((g.newBtn.left + g.newBtn.right) / 2, (g.newBtn.top + g.newBtn.bottom) / 2), 16.0f, 16.0f), brush_.Get());
    DrawText(dc, newChat_ ? L"" : L"", gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL, true), g.newBtn, WithAlpha(kText, (newChat_ ? 1.0f : 0.8f) * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    const std::wstring hint = sendingHere ? L"Sending…" : newChat_ ? (agentTool_ == AgentTool::Claude ? L"Start a new chat with Claude Code…" : L"Start a new chat with Codex…")
                            : (agentTool_ == AgentTool::Claude ? L"Message Claude Code…" : L"Message Codex…");
    DrawField(dc, prompt_, g.box, hint.c_str(), a);
    const bool canSend = !prompt_.text.empty() && !sendingHere;
    brush_->SetColor(D2D1::ColorF(0.04f + (hover_ == kHitSend && canSend ? 0.06f : 0.0f), 0.47f, 1.0f, (canSend ? 1.0f : 0.32f) * a));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((g.send.left + g.send.right) / 2, (g.send.top + g.send.bottom) / 2), 16.0f, 16.0f), brush_.Get());
    DrawText(dc, L"", gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL, true), g.send, WithAlpha(D2D1::ColorF(1, 1, 1, 1), (canSend ? 1.0f : 0.6f) * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!agentError_.empty())                                            // the last prompt could not be delivered
        DrawText(dc, agentError_.c_str(), caption, D2D1::RectF(g.box.left, g.box.top - 18, g.send.right, g.box.top - 2),
                 WithAlpha(D2D1::ColorF(1.0f, 0.55f, 0.50f, 1.0f), a));
}

// ---- typing -----------------------------------------------------------------------------------

void IslandWindow::FocusAgentPrompt() {
    if (suppressed_) return;
    Expand();
    OnClick(static_cast<int>(Section::Ai));
    KillTimer(hwnd_, kCloseTimer);
    Focus(prompt_);
    Animate();
}

void IslandWindow::Focus(TextField& field) {
    if (!field_) {                                                       // the island takes the keyboard
        prevForeground_ = GetForegroundWindow();
        if (prevForeground_ == hwnd_) prevForeground_ = nullptr;
        KillTimer(hwnd_, kCloseTimer);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }
    field_ = &field;
    field.caret = field.text.size();
    UpdateAgentTimer();
    Animate();
}

void IslandWindow::Blur(bool restoreWindow) {
    if (!field_) return;
    field_ = nullptr;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    if (restoreWindow && prevForeground_ && IsWindow(prevForeground_)) SetForegroundWindow(prevForeground_);   // back to what you were doing
    prevForeground_ = nullptr;
    UpdateAgentTimer();
    if (!CursorInIsland(kLeaveSlack)) ShowPending();
    if (expanded_ && !Pinned() && !CursorInIsland(kLeaveSlack)) SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);
    Animate();
}

void IslandWindow::OnChar(wchar_t ch) {
    if (!field_) return;
    if (ch == 0x1B) {                                                   // Esc: leave the box, back to your editor
        Blur(true);
    } else if (ch == L'\r' && field_ == &prompt_) {
        if (GetKeyState(VK_SHIFT) & 0x8000) prompt_.Insert(L"\n");
        else SendPrompt();
    } else if (ch == L'\r' || ch == L'\t') {
        ViewKey(ch == L'\r' ? VK_RETURN : VK_TAB);
    } else if (field_->OnChar(ch)) {
        if (field_ == &prompt_) agentError_.clear();
        if (field_ == &reminderWhat_) reminderFlip_ = false;                // a new time: the guess starts again
    }
    Animate();
}

void IslandWindow::OnKey(WPARAM vk) {
    if (!field_) return;
    if (field_ == &prompt_ && (vk == VK_UP || vk == VK_DOWN)) {          // scroll through what the agent did
        if (const AgentSession* s = CurrentSession())
            agentScroll_ = std::clamp(agentScroll_ + (vk == VK_UP ? 1 : -1), 0, std::max(0, static_cast<int>(s->feed.size()) - 1));
    } else if (field_ != &prompt_ && (vk == VK_UP || vk == VK_DOWN)) {
        ViewKey(vk);
    } else {
        field_->OnKey(vk, hwnd_);
    }
    Animate();
}

bool IslandWindow::ViewKey(WPARAM vk) { return RemindersKey(vk) || SafeShareKey(vk); }

int IslandWindow::FieldSection(const TextField* field) const {
    if (!field) return -1;
    if (field == &prompt_) return static_cast<int>(Section::Ai);
    if (field == &reminderWhat_ || field == &reminderNote_) return static_cast<int>(Section::Reminders);
    if (field == &shareName_ || field == &shareEmail_ || field == &sharePassword_) return static_cast<int>(Section::SafeShare);
    return -1;
}

// One line of editable text: hint when empty, the text scrolled so the caret stays in view, a blinking caret.
void IslandWindow::DrawField(ID2D1DeviceContext* dc, TextField& field, const D2D1_RECT_F& box, const wchar_t* hint, float a, float right,
                             bool secret) {
    const bool focused = field_ == &field;
    const float radius = (box.bottom - box.top) / 2;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (focused ? 0.13f : 0.075f) * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{box, radius, radius}, brush_.Get());
    if (focused) {
        brush_->SetColor(D2D1::ColorF(kAccent.r, kAccent.g, kAccent.b, 0.85f * a));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f), radius, radius},
                                 brush_.Get(), 1.2f);
    }
    IDWriteTextFormat* input = gfx_.Text(13.5f, DWRITE_FONT_WEIGHT_NORMAL);
    const float textLeft = box.left + 14, textRight = box.right - 12 - right, width = textRight - textLeft;
    std::wstring shown = secret ? std::wstring(field.text.size(), L'\u2022') : field.text;
    for (wchar_t& c : shown)
        if (c == L'\n') c = L'⏎';
    if (shown.empty()) {
        DrawText(dc, hint, input, D2D1::RectF(textLeft, box.top, textRight, box.bottom), WithAlpha(kTextDim, 1.1f * a));
    } else {
        const float caretX = std::ceil(gfx_.MeasureWidth(shown.substr(0, field.caret).c_str(), input));
        if (caretX - field.scroll > width - 6) field.scroll = caretX - (width - 6);
        if (caretX < field.scroll) field.scroll = caretX;
        if (field.scroll < 0) field.scroll = 0;
        dc->PushAxisAlignedClip(D2D1::RectF(textLeft, box.top, textRight, box.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        const float full = std::ceil(gfx_.MeasureWidth(shown.c_str(), input)) + 40;
        DrawText(dc, shown.c_str(), input, D2D1::RectF(textLeft - field.scroll, box.top, textLeft - field.scroll + full, box.bottom), WithAlpha(kText, a));
        dc->PopAxisAlignedClip();
    }
    if (focused && (GetTickCount64() / 530) % 2 == 0) {                 // the caret
        const float cx = textLeft + std::ceil(gfx_.MeasureWidth(shown.substr(0, field.caret).c_str(), input)) - field.scroll;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * a));
        dc->FillRectangle(D2D1::RectF(cx, box.top + 8, cx + 1.3f, box.bottom - 8), brush_.Get());
    }
}

void IslandWindow::SendPrompt() {
    std::wstring text = prompt_.text;
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\n')) text.pop_back();
    if (text.empty() || !agentSend_ || (agents_.sending && agents_.sendingTool == agentTool_)) return;
    const AgentSession* s = CurrentSession();
    std::wstring cwd = s ? s->cwd : L"";
    if (cwd.empty() && !agents_.sessions.empty()) cwd = agents_.sessions.front().cwd;          // a new chat: where you worked last
    std::wstring error;
    if (agentSend_(agentTool_, newChat_ || !s ? L"" : s->id, cwd, text, newChat_ || !s, error)) {
        prompt_.Clear();
        agentError_.clear();
        agentScroll_ = 0;
        newChat_ = false;
        agents_.sending = true;                                          // shown at once; the monitor confirms
        agents_.sendingTool = agentTool_;
    } else {
        agentError_ = error;
    }
    Animate();
}

// ---- live pills ------------------------------------------------------------------------------

float IslandWindow::CollapsedW() const {
    if (TourInvite()) return TourInviteW();
    if (Live()) return kLiveW + GlimpseIconsW();
    if (MusicPill()) {                                              // as wide as the line, within reason
        const std::wstring& text = lyrics_.lines[static_cast<size_t>(lyricLine_)].text;
        const float w = text.empty() ? 40.0f : gfx_.MeasureWidth(text.c_str(), gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        return std::clamp(std::ceil(w) + 64.0f, 150.0f, 460.0f) + GlimpseIconsW();
    }
    return kCollapsedW;
}
float IslandWindow::CollapsedH() const { return TourInvite() || Live() ? kLiveH : MusicPill() ? kLyricPillH : kCollapsedH; }

// ---- live lyrics ----------------------------------------------------------------------------

bool IslandWindow::LyricsReady() const {
    return lyricsOn_ && media_.hasSession && lyrics_.status == Lyrics::Status::Found && !lyrics_.lines.empty();
}

void IslandWindow::SetGlimpsePrefs(bool lyrics, bool agents) {
    glimpseLyrics_ = lyrics;
    glimpseAgents_ = agents;
    RefreshCollapsed();
    Animate();
}

bool IslandWindow::MusicGlimpse() const {
    if (!glimpseLyrics_ || !media_.hasSession || !media_.playing || SoluActive()) return false;   // (a conversation hides it)
    return LyricsReady() && lyricLine_ >= 0 && lyricLine_ < static_cast<int>(lyrics_.lines.size());   // only the lyrics, once the singing starts
}

bool IslandWindow::MusicPill() const { return MusicGlimpse() && FrontGlimpse() == L"music"; }

std::vector<IslandWindow::Glimpse> IslandWindow::Glimpses() const {
    std::vector<Glimpse> all;
    for (const Activity& a : activities_) {
        const D2D1_COLOR_F tint = D2D1::ColorF(((a.accent >> 16) & 0xFF) / 255.0f, ((a.accent >> 8) & 0xFF) / 255.0f, (a.accent & 0xFF) / 255.0f, 1.0f);
        all.push_back({a.id, a.glyph, tint});
    }
    if (MusicGlimpse()) all.push_back({L"music", L"\uEC4F", kGreen});
    return all;
}

std::wstring IslandWindow::FrontGlimpse() const {
    const auto all = Glimpses();
    if (all.empty()) return {};
    for (const Glimpse& g : all)
        if (g.id == frontGlimpse_) return g.id;                  // the user's pick, while it lasts
    return all.front().id;
}

const Activity* IslandWindow::FrontActivity() const {
    const std::wstring front = FrontGlimpse();
    for (const Activity& a : activities_)
        if (a.id == front) return &a;
    return nullptr;
}

float IslandWindow::GlimpseIconsW() const {
    const size_t others = std::min<size_t>(3, Glimpses().size() > 0 ? Glimpses().size() - 1 : 0);
    return others ? others * 24.0f + 6.0f : 0.0f;
}

std::vector<D2D1_RECT_F> IslandWindow::GlimpseIcons(const D2D1_RECT_F& pill) const {
    std::vector<D2D1_RECT_F> out;
    const float cy = (pill.top + pill.bottom) / 2;
    const size_t others = static_cast<size_t>(GlimpseIconsW() / 24.0f);
    for (size_t i = 0; i < others; ++i) {
        const float right = pill.right - 10.0f - i * 24.0f;
        out.push_back(D2D1::RectF(right - 20.0f, cy - 10.0f, right, cy + 10.0f));
    }
    return out;
}

void IslandWindow::DrawGlimpseIcons(ID2D1DeviceContext* dc, const D2D1_RECT_F& pill, float a) {
    const auto all = Glimpses();
    const auto icons = GlimpseIcons(pill);
    if (icons.empty()) return;
    const std::wstring front = FrontGlimpse();
    size_t k = 0;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * a));                // a hairline between what is shown and the rest
    const float sepX = icons.back().left - 5.0f, cy = (pill.top + pill.bottom) / 2;
    dc->DrawLine(D2D1::Point2F(sepX, cy - 7), D2D1::Point2F(sepX, cy + 7), brush_.Get(), 1.0f);
    for (const Glimpse& g : all) {
        if (g.id == front) continue;
        if (k >= icons.size()) break;
        const D2D1_RECT_F& r = icons[k++];
        brush_->SetColor(D2D1::ColorF(g.tint.r, g.tint.g, g.tint.b, 0.20f * a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), 9.5f, 9.5f), brush_.Get());
        DrawText(dc, g.glyph.c_str(), gfx_.Text(10.0f, DWRITE_FONT_WEIGHT_NORMAL, true), r, WithAlpha(g.tint, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void IslandWindow::OnMiddleClick(POINT screen) {
    POINT cl = screen;
    ScreenToClient(hwnd_, &cl);
    const D2D1_POINT_2F p = ToDip(cl);
    const D2D1_RECT_F r = IslandRect();
    auto in = [&](const D2D1_RECT_F& b) { return p.x >= b.left - 2 && p.x <= b.right + 2 && p.y >= b.top - 6 && p.y <= b.bottom + 6; };
    const auto icons = GlimpseIcons(r);
    size_t k = 0;
    const std::wstring front = FrontGlimpse();
    for (const Glimpse& g : Glimpses()) {                          // an icon: that one to the front
        if (g.id == front) continue;
        if (k >= icons.size()) break;
        if (in(icons[k++])) {
            frontGlimpse_ = g.id;
            SetActivities(activities_);                          // (the pill's shape and timers follow)
            return;
        }
    }
    if (const Activity* shown = FrontActivity(); shown && shown->dismissible) {
        D2D1_RECT_F x = LiveClose(r);
        x.right -= GlimpseIconsW(), x.left -= GlimpseIconsW();
        if (in(x)) {                                             // its close mark: eject a drive, dismiss anything else
            if (shown->mark == Activity::Mark::Close) OnClick(kHitLiveClose);
            else if (mark_) mark_(*shown);
            return;
        }
    }
    if (const Activity* shown = FrontActivity(); shown && shown->mark == Activity::Mark::Copy) {   // a login code: anywhere copies it
        if (mark_) mark_(*shown);
        return;
    }
    if (const Activity* shown = FrontActivity(); shown && !shown->open.empty()) {   // a drive: open it
        ShellExecuteW(nullptr, L"open", shown->open.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (Live() || MusicPill()) OnClick(kHitLive);                  // the pill: open on what it is about
    else Expand();
}

float IslandWindow::LyricShift() const { return section_ == Section::Music && LyricsReady() ? kLyricLinesH : 0.0f; }

void IslandWindow::RefreshCollapsed() {
    if (expanded_) return;
    const float w = CollapsedW(), h = CollapsedH();
    if (w == w_.target && h == h_.target) return;
    SetOpenSprings(w_, h_);                                         // the pill morphs to the new line's width
    w_.target = w;
    h_.target = h;
    SetHitRegion(false);
    Animate();
}

void IslandWindow::UpdateLyricLine() {
    KillTimer(hwnd_, kLyricsTimer);
    constexpr double kLead = 0.15;                                  // a line appears a moment before it is sung
    const double pos = media_.PositionNow() + kLead;
    const int line = LyricsReady() ? lyrics_.LineAt(pos) : -1;
    if (line != lyricLine_) {
        lyricPrev_ = lyricLine_;
        lyricLine_ = line;
        lyricT_ = 0.0f;
        Animate();
    }
    RefreshCollapsed();
    if (LyricsReady() && media_.playing && line + 1 < static_cast<int>(lyrics_.lines.size())) {   // wake exactly for the next line
        const double wait = lyrics_.lines[static_cast<size_t>(line + 1)].at - pos;
        SetTimer(hwnd_, kLyricsTimer, static_cast<UINT>(std::clamp(wait * 1000.0, 15.0, 60000.0)), nullptr);
    }
}

void IslandWindow::SetLyrics(const Lyrics& lyrics) {
    const bool same = lyrics.key == lyrics_.key && lyrics.status == lyrics_.status && lyrics.lines.size() == lyrics_.lines.size();
    lyrics_ = lyrics;
    if (!same) lyricLine_ = lyricPrev_ = -1;
    UpdateLyricLine();
    ApplyHeightTarget();
    Animate();
}

void IslandWindow::SetLyricsOn(bool on) {
    if (lyricsOn_ == on) return;
    lyricsOn_ = on;
    UpdateLyricLine();
    ApplyHeightTarget();
    Animate();
}

D2D1_POINT_2F IslandWindow::LyricsButton(const D2D1_RECT_F& island) const {
    const float x1 = island.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    return D2D1::Point2F(x1 - 10.0f, island.top + kControlsY + LyricShift());
}

// The closed island as a slim pill with the line being sung: the old line drifts up and out, the new one rises in.
void IslandWindow::DrawMusicPill(ID2D1DeviceContext* dc, const D2D1_RECT_F& full, float a) {
    if (a < 0.01f) return;
    const D2D1_RECT_F r = D2D1::RectF(full.left, full.top, full.right - GlimpseIconsW(), full.bottom);   // (the icons sit at the end)
    if (!LyricsReady() || lyricLine_ < 0) return;                    // (fading out as the lyrics end)
    const float cy = (r.top + r.bottom) / 2;
    DrawText(dc, L"\u266A", gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 10, cy - 9, r.left + 28, cy + 9),
             WithAlpha(kGreen, 0.9f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat* font = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const D2D1_RECT_F box = D2D1::RectF(r.left + 30, r.top, r.right - 16, r.bottom);
    const float t = SmoothStep(0.0f, 1.0f, lyricT_);
    auto line = [&](int i) -> std::wstring {
        if (i < 0 || i >= static_cast<int>(lyrics_.lines.size())) return {};
        const std::wstring& s = lyrics_.lines[static_cast<size_t>(i)].text;
        return s.empty() ? L"\u266A \u266A \u266A" : s;
    };
    dc->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (t < 1.0f && lyricPrev_ >= 0) {
        const std::wstring old = line(lyricPrev_);
        DrawText(dc, old.c_str(), font, D2D1::RectF(box.left, cy - 9 - 8 * t, box.right, cy + 9 - 8 * t), WithAlpha(kText, (1 - t) * a),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    const std::wstring now = line(lyricLine_);
    DrawText(dc, now.c_str(), font, D2D1::RectF(box.left, cy - 9 + 8 * (1 - t), box.right, cy + 9 + 8 * (1 - t)), WithAlpha(kText, t * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->PopAxisAlignedClip();
}

// The Music tab: the line being sung, bright, and the next one, quiet. The same rise as the pill when a line changes.
void IslandWindow::DrawLyricLines(ID2D1DeviceContext* dc, float x0, float x1, float top, float a) {
    const int n = static_cast<int>(lyrics_.lines.size());
    auto line = [&](int i) -> std::wstring {
        if (i < 0) return L"\u266A";                                 // before the singing starts
        if (i >= n) return {};
        const std::wstring& s = lyrics_.lines[static_cast<size_t>(i)].text;
        return s.empty() ? L"\u266A \u266A \u266A" : s;
    };
    const float t = SmoothStep(0.0f, 1.0f, lyricT_);
    IDWriteTextFormat* nextFont = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    auto fit = [&](const std::wstring& s) {                            // a long line steps down a size before it is cut short
        IDWriteTextFormat* f = gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        return gfx_.MeasureWidth(s.c_str(), f) <= x1 - x0 ? f : gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    };
    dc->PushAxisAlignedClip(D2D1::RectF(x0 - 2, top - 2, x1 + 2, top + kLyricLinesH), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (t < 1.0f && lyricPrev_ != lyricLine_) {
        const std::wstring old = line(lyricPrev_);
        DrawText(dc, old.c_str(), fit(old), D2D1::RectF(x0, top - 10 * t, x1, top + 19 - 10 * t), WithAlpha(kText, (1 - t) * a));
    }
    const std::wstring now = line(lyricLine_), next = line(lyricLine_ + 1);
    DrawText(dc, now.c_str(), fit(now), D2D1::RectF(x0, top + 18 * (1 - t), x1, top + 19 + 18 * (1 - t)),
             WithAlpha(kText, (lyricLine_ < 0 ? 0.5f : 1.0f) * (0.45f + 0.55f * t) * a));
    DrawText(dc, next.c_str(), nextFont, D2D1::RectF(x0, top + 20, x1, top + 35), WithAlpha(kTextDim, 1.05f * t * a));
    dc->PopAxisAlignedClip();
}

D2D1_RECT_F IslandWindow::LiveClose(const D2D1_RECT_F& pill) const {
    const float cy = (pill.top + pill.bottom) / 2;
    return D2D1::RectF(pill.right - 30, cy - 11, pill.right - 8, cy + 11);
}

void IslandWindow::SetActivities(const std::vector<Activity>& items) {
    activities_ = items;
    std::erase_if(thumbs_, [&](const auto& t) {                          // (the pictures of pills that are gone)
        return std::none_of(activities_.begin(), activities_.end(), [&](const Activity& x) { return x.thumb.get() == t.first; });
    });
    RefreshCollapsed();                                                  // grow into a pill, change it, or shrink back
    KillTimer(hwnd_, kLiveTimer);
    if (const Activity* shown = FrontActivity()) {
        const Activity& a = *shown;
        const bool moving = a.kind == Activity::Kind::Progress && a.progress < 0;    // the indeterminate shimmer
        const bool arriving = a.startedAt && GetTickCount64() - a.startedAt < kArrivalMs;   // just plugged in: the animation
        SetTimer(hwnd_, kLiveTimer, arriving ? 16 : moving ? 50 : 1000, nullptr);
    }
    Animate();
}

void IslandWindow::DrawLive(ID2D1DeviceContext* dc, const D2D1_RECT_F& full, float a) {
    const Activity* shown = FrontActivity();
    if (a < 0.01f || !shown) return;
    const Activity& act = *shown;
    const D2D1_RECT_F r = D2D1::RectF(full.left, full.top, full.right - GlimpseIconsW(), full.bottom);   // (the icons sit at the end)
    const D2D1_COLOR_F accent = D2D1::ColorF(((act.accent >> 16) & 0xFF) / 255.0f, ((act.accent >> 8) & 0xFF) / 255.0f,
                                             (act.accent & 0xFF) / 255.0f, 1.0f);
    const float cy = (r.top + r.bottom) / 2;
    const uint64_t now = GetTickCount64();

    // Icon in a soft accent circle. Just plugged in: it pops in, a ring pulses out once and the words slide in.
    const float arrive = act.startedAt ? Clamp01(static_cast<float>(now - act.startedAt) / kArrivalMs) : 1.0f;
    const float pop = 1.0f + 0.45f * std::sin(3.14159f * std::min(1.0f, arrive * 1.6f)) * (1.0f - arrive);
    if (arrive < 1.0f) {
        brush_->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.55f * (1.0f - arrive) * a));
        const float ring = 10.5f + 16.0f * SmoothStep(0.0f, 1.0f, arrive);
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 19, cy), ring, ring), brush_.Get(), 1.6f);
    }
    float textLeft = r.left + 36;
    if (ID2D1Bitmap1* thumb = act.thumb && !act.thumb->empty() ? Thumb(*act.thumb) : nullptr) {   // a screenshot: the picture itself
        // The picture takes the island's own shape: a capsule the same distance from the top, bottom and left edges,
        // its rounded end concentric with the pill's, so it never runs into the curve.
        const float h0 = std::min(24.0f, (r.bottom - r.top) - 12.0f), inset = ((r.bottom - r.top) - h0) / 2;
        const float w0 = std::clamp(h0 * act.thumb->width / std::max(1u, act.thumb->height), h0, 40.0f);
        const float cx = r.left + inset + w0 / 2, h = h0 * pop, w = w0 * pop;
        const D2D1_RECT_F pic = D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
        textLeft = r.left + inset + w0 + 9;
        Com<ID2D1RoundedRectangleGeometry> clip;
        gfx_.d2dFactory()->CreateRoundedRectangleGeometry(D2D1_ROUNDED_RECT{pic, h / 2, h / 2}, &clip);
        dc->PushLayer(D2D1::LayerParameters1(pic, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
        dc->DrawBitmap(thumb, pic, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        dc->PopLayer();
    } else {
        brush_->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.22f * a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 19, cy), 10.5f * pop, 10.5f * pop), brush_.Get());
        DrawText(dc, act.glyph.c_str(), gfx_.Text(11.0f * pop, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(r.left + 4, cy - 12, r.left + 34, cy + 12),
                 WithAlpha(accent, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    const float slide = (1.0f - SmoothStep(0.15f, 1.0f, arrive)) * 12.0f, textA = a * SmoothStep(0.1f, 0.8f, arrive);

    // Right side: countdown ring, percentage, or a close button.
    float right = r.right - 12;
    if (act.kind == Activity::Kind::Timer && act.length) {
        const float left = act.endsAt > now ? static_cast<float>(act.endsAt - now) : 0.0f;
        const int secs = static_cast<int>((left + 999) / 1000);
        wchar_t clock[16];
        swprintf_s(clock, L"%d:%02d", secs / 60, secs % 60);
        const D2D1_POINT_2F c = D2D1::Point2F(r.right - 20, cy);
        DrawRing(dc, c, 8.0f, 2.2f, 1.0f - left / static_cast<float>(act.length), accent, a, 0.18f);
        DrawText(dc, clock, gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.right - 92, cy - 9, r.right - 34, cy + 9),
                 WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        right = r.right - 96;
    } else if (act.dismissible) {
        const D2D1_RECT_F x = LiveClose(r);
        if (act.mark == Activity::Mark::Eject)                            // a drive: its mark ejects it
            DrawText(dc, L"\u23CF", gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_NORMAL), x, WithAlpha(kText, 0.85f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        else if (act.mark == Activity::Mark::Save)                        // a screenshot: its mark keeps it in Pictures\Screenshots
            DrawText(dc, L"\uE74E", gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL, true), x, WithAlpha(kText, 0.85f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        else if (act.mark == Activity::Mark::Copy)                        // a login code: its mark copies it
            DrawText(dc, L"\uE8C8", gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL, true), x, WithAlpha(kText, 0.85f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        else
            DrawText(dc, L"", gfx_.Text(10.0f, DWRITE_FONT_WEIGHT_NORMAL, true), x, WithAlpha(kTextDim, 1.2f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        right = x.left - 4;
    } else if (act.progress >= 0) {
        wchar_t pct[8];
        swprintf_s(pct, L"%d%%", static_cast<int>(act.progress * 100 + 0.5f));
        DrawText(dc, pct, gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.right - 60, cy - 9, r.right - 14, cy + 9),
                 WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        right = r.right - 64;
    }

    // Title and detail on one line.
    IDWriteTextFormat* strong = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float titleW = std::ceil(gfx_.MeasureWidth(act.title.c_str(), strong));
    textLeft += slide;
    DrawText(dc, act.title.c_str(), strong, D2D1::RectF(textLeft, cy - 9, std::min(right, textLeft + titleW + 2), cy + 9), WithAlpha(kText, textA));
    if (!act.detail.empty() && textLeft + titleW + 10 < right)
        DrawText(dc, act.detail.c_str(), gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(textLeft + titleW + 8, cy - 9, right, cy + 9),
                 WithAlpha(kTextDim, 1.15f * textA));

    // A thin progress line along the bottom (a moving shimmer when the size is unknown).
    if (act.kind == Activity::Kind::Progress) {
        const float x0 = r.left + 16, x1 = r.right - 16, y = r.bottom - 3.0f;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, y - 1, x1, y + 1), 1, 1}, brush_.Get());
        brush_->SetColor(WithAlpha(accent, 0.95f * a));
        if (act.progress >= 0) {
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, y - 1, x0 + (x1 - x0) * Clamp01(act.progress), y + 1), 1, 1}, brush_.Get());
        } else {
            const float phase = static_cast<float>(now % 1400) / 1400.0f, w = (x1 - x0) * 0.28f;
            const float s = x0 + (x1 - x0 + w) * phase - w;
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(std::max(x0, s), y - 1, std::min(x1, s + w), y + 1), 1, 1}, brush_.Get());
        }
    }
    if (SoluAwake()) {                                                   // Solu is still there: a tiny green dot
        brush_->SetColor(D2D1::ColorF(kGreen.r, kGreen.g, kGreen.b, 0.85f * a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 27, cy - 7), 2.2f, 2.2f), brush_.Get());
    }
}

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

    // Moods: sleepy "z"s drifting up, a worried sweat drop, sparkles while it cheers.
    if (mascot_->talk() == Mascot::Talk::None && mascot_->state() == State::Idle) {
        if (mascot_->mood() == Mascot::Mood::Sleepy) {
            const D2D1_POINT_2F from = MascotPoint(0.30f, 0.20f);
            for (int i = 0; i < 3; ++i) {
                const float phase = std::fmod(t * 0.28f + i / 3.0f, 1.0f);
                const float a = std::sin(phase * 3.14159f) * 0.8f * opacity;
                const float x = from.x + phase * 14.0f, y = from.y - phase * 24.0f;
                DrawText(dc, L"z", gfx_.Text(9.0f + 5.0f * phase, DWRITE_FONT_WEIGHT_BOLD), D2D1::RectF(x - 8, y - 9, x + 8, y + 9),
                         D2D1::ColorF(0.80f, 0.76f, 1.0f, a), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        } else if (mascot_->mood() == Mascot::Mood::Worried) {
            const D2D1_POINT_2F at = MascotPoint(0.26f, 0.22f);
            const float slide = std::fmod(t * 0.5f, 1.0f) * 6.0f;
            brush_->SetColor(D2D1::ColorF(0.55f, 0.80f, 1.0f, 0.85f * opacity));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(at.x, at.y + slide), 2.6f, 3.4f), brush_.Get());
        }
    }
    if (mascot_->state() == State::Notify && std::string(mascot_->Face()) == "happy") {
        const D2D1_POINT_2F at = MascotPoint(0.0f, 0.34f);
        for (int i = 0; i < 4; ++i) {
            const float ang = i * 1.57f + st * 2.0f, rad = 16.0f + st * 18.0f;
            const float a = std::max(0.0f, 1.0f - st / 1.1f) * opacity;
            DrawText(dc, L"✦", gfx_.Text(10.0f, DWRITE_FONT_WEIGHT_NORMAL),
                     D2D1::RectF(at.x + std::cos(ang) * rad - 8, at.y + std::sin(ang) * rad * 0.6f - 8, at.x + std::cos(ang) * rad + 8,
                                 at.y + std::sin(ang) * rad * 0.6f + 8), D2D1::ColorF(1.0f, 0.86f, 0.40f, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
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
        return field_ ? MA_ACTIVATE : MA_NOACTIVATE;   // clicking the island never steals focus, except while typing in it
    case WM_CHAR:
        OnChar(static_cast<wchar_t>(wp));
        return 0;
    case WM_KEYDOWN:
        OnKey(wp);
        return 0;
    case WM_KILLFOCUS:
        if (field_) Blur(false);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_MOUSEMOVE:
        if (seeking_) {                                    // dragging the progress bar: follow the pointer, even outside it
            seekFraction_ = SeekFractionAt(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}).x, IslandRect());
            Animate();
            return 0;
        }
        if (shelfPressed_ >= 0 && (wp & MK_LBUTTON)) {                              // pulled an item off the shelf: a real drag begins
            const int dx = GET_X_LPARAM(lp) - shelfPressPt_.x, dy = GET_Y_LPARAM(lp) - shelfPressPt_.y;
            if (std::abs(dx) > GetSystemMetrics(SM_CXDRAG) || std::abs(dy) > GetSystemMetrics(SM_CYDRAG)) {
                const int index = shelfPressed_;
                shelfPressed_ = -1;
                StartShelfDrag(index);
                return 0;
            }
        }
        OnMouseMove(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        return 0;
    case WM_APP_CLICKAWAY:                                         // a click outside the island while the live view is up: tuck away, keep talking
        if (expanded_ && CaptionsShown() && !CursorInIsland(3.0f) && !(mascot_ && mascot_->permissionPending())) {
            captionsYielded_ = true;
            Log("island: live view closed by a click outside");
            UpdateClickAway();
            Collapse();
        }
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (expanded_ && !field_) ShowPending();
        if (expanded_) {
            if (!Pinned()) SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);
            if (hover_ != -1) { hover_ = -1; Animate(); }
        }
        return 0;
    case WM_APP_ISLAND_MIDDLE:                                    // middle click or wheel on the closed island
        if (wp) wheelOpenedAt_ = GetTickCount64();               // the rest of that wheel flick must not scroll the tab
        if (!expanded_ && !suppressed_) {
            if (TourInvite()) Expand();                              // the tour's invitation: any way in is the right one
            else if (!wp) OnMiddleClick(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});   // a click: it matters where
            else if (Live() || MusicPill()) OnClick(kHitLive);         // the wheel: open on what is shown
            else Expand();
        }
        return 0;
    case WM_MBUTTONDOWN:                                          // ... and on the open island: close it
        if (expanded_ && !Pinned()) Collapse();
        return 0;
    case WM_APP_ISLAND_BUTTON:                                    // a button went down or up anywhere
        if (wp) {
            SetTimer(hwnd_, kHoldTimer, 150, nullptr);   // (the press itself has already reached the app below)
        } else {
            KillTimer(hwnd_, kHoldTimer);
            if (holding_) {
                holding_ = false;
                ApplyClickThrough();
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_RBUTTONUP: {                                           // right-click a round button: quick mute / unmute
        const int hit = HitTest(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        if (expanded_ && hit >= kHitShelfItem && hit < kHitShelfItem + 100) {          // right-click a shelf item: show it in its folder
            OpenShelfItem(hit - kHitShelfItem, true);
            return 0;
        }
        if (expanded_ && hit >= kHitAudioButton && hit < kHitAudioButton + 3) ToggleMute(hit - kHitAudioButton);
        return 0;
    }
    case WM_LBUTTONDOWN:
        pressed_ = HitTest(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        shelfPressed_ = pressed_ >= kHitShelfItem && pressed_ < kHitShelfItem + 100 ? pressed_ - kHitShelfItem : -1;
        shelfPressPt_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
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
        if (!expanded_ && TourInvite()) {                     // a plain click on the invitation opens it too
            Expand();
            return 0;
        }
        const int hit = HitTest(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        const int was = pressed_;
        pressed_ = -1;
        shelfPressed_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (was >= 0 && hit == was) OnClick(hit);
        return 0;
    }
    case WM_CAPTURECHANGED:
        pressed_ = -1;
        seeking_ = false;                                  // capture lost mid-drag: cancel, don't seek
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hover_ == kHitPrompt ? IDC_IBEAM : hover_ >= 0 || TourInvite() ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_TIMER:
        if (wp == kAgentTimer) {
            Animate();                                   // the spinner turns, the caret blinks
            return 0;
        }
        if (wp == kLyricsTimer) {
            UpdateLyricLine();                           // the next line starts now
            return 0;
        }
        if (wp == kLiveTimer) {
            if (const Activity* a = FrontActivity(); a && a->startedAt && GetTickCount64() - a->startedAt >= kArrivalMs)
                SetActivities(activities_);              // the arrival animation is over: back to the calm pace
            Animate();                                   // the countdown or the shimmer moves on
            return 0;
        }
        if (wp == kTourTimer) {                          // the goodbye is over: the island tucks itself away
            KillTimer(hwnd_, kTourTimer);
            if (tourFinishing_) {
                TourLeave(static_cast<TourStep>(tourStep_));
                tourOn_ = tourFinishing_ = false;
                Collapse();
                if (tour_.finished) tour_.finished(true);
            }
            return 0;
        }
        if (wp == kShareTimer) {
            KillTimer(hwnd_, kShareTimer);
            if (GetTickCount64() >= shareConfirmUntil_) shareConfirm_.clear();
            Animate();
            return 0;
        }
        if (wp == kTestTimer) {
            KillTimer(hwnd_, kTestTimer);
            Animate();                                   // the test verdict has expired: show the normal status again
            return 0;
        }
        if (wp == kHoldTimer) {
            KillTimer(hwnd_, kHoldTimer);
            if (!expanded_ && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {   // still held: it may be files on their way to the Shelf
                holding_ = true;
                ApplyClickThrough();
            }
        } else if (wp == kCloseTimer || wp == kAutoCloseTimer) {
            KillTimer(hwnd_, wp);
            if (wp == kAutoCloseTimer) autoClosePending_ = false;
            if (!Pinned()) {
                if (!CursorInIsland(kLeaveSlack)) Collapse();
                else if (!tracking_) SetTimer(hwnd_, kCloseTimer, 150, nullptr);   // left slowly: still at the edge, look again
            }
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
