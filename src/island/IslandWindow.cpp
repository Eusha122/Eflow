#include "island/IslandWindow.h"

#include <d2d1effects.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

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
constexpr float kArtSize = 92.0f;
constexpr float kArtTop = 62.0f;
constexpr float kControlsY = 145.0f;       // centre line of the transport buttons (from the panel top)

const wchar_t* const kTabLabels[3] = {L"Music", L"Notifications", L"AI Requests"};
const wchar_t* const kEmptyTitle[3] = {L"Nothing playing", L"You're all caught up", L"No pending requests"};
const wchar_t* const kEmptyBody[3] = {
    L"Play something in Spotify, YouTube or your browser",
    L"New notifications will show up here",
    L"Claude Code and Codex approvals will appear here",
};

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

bool IslandWindow::Pinned() const {
    // A pending AI permission request keeps the panel open until it is answered.
    return pressed_ >= 0 || (mascot_ && mascot_->permissionPending());
}

std::array<D2D1_RECT_F, 3> IslandWindow::TabRects(const D2D1_RECT_F& island) const {
    IDWriteTextFormat* fmt = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::array<float, 3> widths{};
    float total = 0;
    for (int i = 0; i < 3; ++i) {
        widths[i] = std::ceil(gfx_.MeasureWidth(kTabLabels[i], fmt)) + 2 * kTabPadX;
        total += widths[i];
    }
    total += 2 * kTabGap;
    float x = island.left + ((island.right - island.left) - total) / 2.0f;
    std::array<D2D1_RECT_F, 3> out{};
    for (int i = 0; i < 3; ++i) {
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
    SetOpenSprings(w_, h_);
    w_.target = kExpandedW;
    h_.target = kExpandedH;
    Animate();
}

void IslandWindow::Collapse() {
    if (!expanded_) return;
    expanded_ = false;
    hover_ = pressed_ = -1;
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
        ShowWindow(hwnd_, SW_HIDE);
    } else {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Animate();
    }
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
    if (!mascot_) return;
    // Worth attention: open by itself on the Notifications tab, and close again after a moment unless
    // the user has the pointer on it (or it was already open).
    mascot_->Notify();
    const bool wasOpen = expanded_;
    OnClick(static_cast<int>(Section::Notifications));
    Expand();
    KillTimer(hwnd_, kAutoCloseTimer);
    autoClosePending_ = !wasOpen;
    if (autoClosePending_) SetTimer(hwnd_, kAutoCloseTimer, kAutoCloseMs, nullptr);
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
    for (int i = 0; i < 3; ++i)
        if (p.x >= tabs[i].left && p.x <= tabs[i].right && p.y >= tabs[i].top && p.y <= tabs[i].bottom) return i;
    if (section_ == Section::Music && media_.hasSession) {
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
    const D2D1_RECT_F r = IslandRect();
    const bool inside = p.x >= r.left - kLeaveSlack && p.x <= r.right + kLeaveSlack && p.y <= r.bottom + kLeaveSlack;
    if (inside) KillTimer(hwnd_, kCloseTimer);
    else if (!Pinned()) SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);

    const int hit = HitTest(p);
    if (hit != hover_) {
        hover_ = hit;
        Animate();
    }
}

void IslandWindow::OnClick(int id) {
    if (id >= kHitPrev && id <= kHitNext) {
        const bool enabled = id == kHitPlay ? media_.canToggle : id == kHitNext ? media_.canNext : media_.canPrevious;
        if (!enabled || !mediaHandler_) return;
        if (id == kHitPlay) {                    // flip immediately; the service confirms a moment later
            media_.position = media_.PositionNow();
            media_.positionStamp = GetTickCount64();
            media_.playing = !media_.playing;
        }
        mediaHandler_(id == kHitPlay ? MediaCommand::TogglePlayPause : id == kHitNext ? MediaCommand::Next
                                                                                      : MediaCommand::Previous);
        Animate();
        return;
    }
    if (id < 0 || id > 2) return;
    section_ = static_cast<Section>(id);
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
        moving |= Approach(tabHover_[i], (i == hover_ && i != static_cast<int>(section_)) ? 1.0f : 0.0f, dt, 22.0f);
        moving |= Approach(sectionAlpha_[i], i == static_cast<int>(section_) ? 1.0f : 0.0f, dt, 16.0f);
    }
    return moving;
}

void IslandWindow::OnSettled() {
    if (expanded_) {
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

    const float contentAlpha = SmoothStep(0.62f, 1.0f, progress);
    if (contentAlpha > 0.003f) {
        dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawContent(dc, r, contentAlpha);
        dc->PopAxisAlignedClip();
    }
}

void IslandWindow::DrawText(ID2D1DeviceContext* dc, const wchar_t* text, IDWriteTextFormat* fmt, D2D1_RECT_F r,
                            D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align) {
    if (!fmt) return;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    for (int i = 0; i < 3; ++i) {
        if (tabHover_[i] > 0.0f) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.055f * tabHover_[i] * alpha));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{tabs[i], kTabH / 2, kTabH / 2}, brush_.Get());
        }
        const float selected = sectionAlpha_[i];
        const float a = 0.50f + 0.45f * selected + 0.25f * tabHover_[i];
        DrawText(dc, kTabLabels[i], tabFont, tabs[i], WithAlpha(kText, std::min(a, 0.96f) * alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Section body: empty states for now; cross-fades with a small vertical slide.
    IDWriteTextFormat* titleFont = gfx_.Text(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* bodyFont = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    const float bodyTop = base.top + kTabTop + kTabH + 18.0f;
    const float bodyBottom = base.bottom - 18.0f;
    const float mid = (bodyTop + bodyBottom) / 2.0f;
    for (int i = 0; i < 3; ++i) {
        const float a = sectionAlpha_[i] * alpha;
        if (a < 0.003f) continue;
        const float slide = (1.0f - sectionAlpha_[i]) * 8.0f;
        if (i == static_cast<int>(Section::Music) && media_.hasSession) {
            DrawMusic(dc, D2D1::RectF(base.left, base.top + slide, base.right, base.bottom + slide), a);
            continue;
        }
        const float right = base.right - (mascot_ ? kMascotLane : 24.0f);
        DrawText(dc, kEmptyTitle[i], titleFont, D2D1::RectF(base.left + 24, mid - 26 + slide, right, mid + slide),
                 WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, kEmptyBody[i], bodyFont, D2D1::RectF(base.left + 24, mid + 2 + slide, right, mid + 24 + slide),
                 WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

// ---- music -----------------------------------------------------------------------------------

void IslandWindow::SetMedia(const MediaInfo& media) {
    media_ = media;
    Animate();
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
    DrawText(dc, subtitle.c_str(), gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL),
             D2D1::RectF(x0, base.top + 84, x1, base.top + 102), WithAlpha(kTextDim, alpha));

    // Progress.
    const float barY = base.top + 113;
    const D2D1_ROUNDED_RECT track{D2D1::RectF(x0, barY, x1, barY + 3), 1.5f, 1.5f};
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.14f * alpha));
    dc->FillRoundedRectangle(track, brush_.Get());
    const double pos = media_.PositionNow();
    if (media_.duration > 0) {
        const float f = static_cast<float>(std::clamp(pos / media_.duration, 0.0, 1.0));
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.88f * alpha));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, barY, x0 + (x1 - x0) * f, barY + 3), 1.5f, 1.5f},
                                 brush_.Get());
        IDWriteTextFormat* timeFont = gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL);
        DrawText(dc, FormatTime(pos).c_str(), timeFont, D2D1::RectF(x0, barY + 5, x0 + 60, barY + 19),
                 WithAlpha(kTextDim, alpha));
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
    if (mascot_->baseState() == State::Music) {
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
    case WM_LBUTTONDOWN:
        pressed_ = HitTest(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        if (pressed_ >= 0) SetCapture(hwnd_);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitTest(ToDip(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
        const int was = pressed_;
        pressed_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (was >= 0 && hit == was) OnClick(hit);
        return 0;
    }
    case WM_CAPTURECHANGED:
        pressed_ = -1;
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hover_ >= 0 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_TIMER:
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
