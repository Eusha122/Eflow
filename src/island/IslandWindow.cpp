#include "island/IslandWindow.h"

#include <d2d1effects.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/Messages.h"

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

constexpr D2D1_COLOR_F Rgba(float r, float g, float b, float a) { return {r, g, b, a}; }
constexpr D2D1_COLOR_F kIslandFill = Rgba(0.051f, 0.051f, 0.063f, 1.0f);
constexpr D2D1_COLOR_F kText = Rgba(0.96f, 0.96f, 0.98f, 1.0f);
constexpr D2D1_COLOR_F kTextDim = Rgba(0.96f, 0.96f, 0.98f, 0.52f);

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

IslandWindow::IslandWindow(Gfx& gfx, HMONITOR monitor, HWND owner) : gfx_(gfx), monitor_(monitor), owner_(owner) {
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
    Render();
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
    pxH_ = static_cast<UINT>(std::ceil((kTopInset + kExpandedH + kMargin) * scale_));
    const int x = m.left + ((m.right - m.left) - static_cast<int>(pxW_)) / 2;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, m.top, static_cast<int>(pxW_), static_cast<int>(pxH_), SWP_NOACTIVATE);
}

D2D1_RECT_F IslandWindow::IslandRect() const {
    const float winW = pxW_ / scale_;
    const float x = (winW - w_.x) / 2.0f;
    return D2D1::RectF(x, kTopInset, x + w_.x, kTopInset + h_.x);
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
        const float y1 = kTopInset + kCollapsedH + kHotSlackY;
        rgn = CreateRectRgn(static_cast<int>(x0 * scale_), 0, static_cast<int>(std::ceil(x1 * scale_)),
                            static_cast<int>(std::ceil(y1 * scale_)));
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
    SetCloseSprings(w_, h_);
    w_.target = kCollapsedW;
    h_.target = kCollapsedH;
    Animate();
}

void IslandWindow::SetSuppressed(bool suppressed) {
    if (suppressed == suppressed_) return;
    suppressed_ = suppressed;
    if (suppressed) {
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
        Render();
    }
}

int IslandWindow::HitTest(D2D1_POINT_2F p) const {
    if (!expanded_ || h_.x < kExpandedH * 0.85f) return -1;
    const auto tabs = TabRects(IslandRect());
    for (int i = 0; i < 3; ++i)
        if (p.x >= tabs[i].left && p.x <= tabs[i].right && p.y >= tabs[i].top && p.y <= tabs[i].bottom) return i;
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
    else SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);

    const int hit = HitTest(p);
    if (hit != hover_) {
        hover_ = hit;
        Animate();
    }
}

void IslandWindow::OnClick(int id) {
    if (id < 0 || id > 2) return;
    section_ = static_cast<Section>(id);
    const auto tabs = TabRects(D2D1::RectF(0, 0, kExpandedW, kExpandedH));
    tabX_.target = tabs[id].left;
    tabW_.target = tabs[id].right - tabs[id].left;
    Animate();
}

// ---- animation -------------------------------------------------------------------------------

void IslandWindow::Animate() {
    if (frameQueued_) return;
    frameQueued_ = true;
    QueryPerformanceCounter(&lastFrame_);
    PostMessageW(hwnd_, WM_APP_FRAME, 0, 0);
}

void IslandWindow::Frame() {
    frameQueued_ = false;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = std::clamp(static_cast<float>(now.QuadPart - lastFrame_.QuadPart) / freq.QuadPart, 1.0f / 480, 1.0f / 20);
    lastFrame_ = now;

    const bool moving = StepAnimation(dt);
    Render();                                   // Present(1) paces this loop to the display refresh
    if (moving && !deviceLost_) {
        frameQueued_ = true;
        PostMessageW(hwnd_, WM_APP_FRAME, 0, 0);
    } else {
        OnSettled();
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
        moving |= Approach(tabHover_[i], (i == hover_ && i != static_cast<int>(section_)) ? 1.0f : 0.0f, dt, 22.0f);
        moving |= Approach(sectionAlpha_[i], i == static_cast<int>(section_) ? 1.0f : 0.0f, dt, 16.0f);
    }
    return moving;
}

void IslandWindow::OnSettled() {
    if (expanded_) {
        if (!CursorInIsland(kLeaveSlack) && pressed_ < 0) SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);
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

    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (!suppressed_) DrawIsland(dc_.Get(), progress);
    HRESULT hr = dc_->EndDraw();
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
        DrawText(dc, kEmptyTitle[i], titleFont, D2D1::RectF(base.left + 24, mid - 26 + slide, base.right - 24, mid + slide),
                 WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, kEmptyBody[i], bodyFont, D2D1::RectF(base.left + 24, mid + 2 + slide, base.right - 24, mid + 24 + slide),
                 WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_CENTER);
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
            if (pressed_ < 0) SetTimer(hwnd_, kCloseTimer, kCloseDelayMs, nullptr);
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
        } else if (wp == kCloseTimer) {
            KillTimer(hwnd_, kCloseTimer);
            if (pressed_ < 0 && !CursorInIsland(kLeaveSlack)) Collapse();
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
