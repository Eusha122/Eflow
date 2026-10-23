// The welcome tour: the closed island invites the user to open it (scroll or middle-click: the first lesson), then the
// panel walks through Eflow in seven steps. Each step has a scene on the left, acted out on a little desktop (a cursor
// that moves, clicks and drags; the island opening, notifications sliding in, a file on its way to a phone), the words
// and the step's one action in the middle, and the cat on the right, reacting. Every permission is asked for at the
// step that explains it, and shows its real state (on, waiting for Windows, or off with a way to fix it).
#include "island/IslandWindow.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/Log.h"
#include "island/IslandStyle.h"

namespace eflow {
namespace {

constexpr float kPi = 3.14159265f;
constexpr int kTourSteps = 7;                    // Hello .. Done (the invite is not a step)

// ---- motion ------------------------------------------------------------------------------------------------------
float Seg(float t, float a, float b) { return Clamp01((t - a) / (b - a)); }
float EaseOut(float t) {
    t = Clamp01(t);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
float EaseInOut(float t) {
    t = Clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}
float EaseOutBack(float t) {                     // overshoots a little, then settles: things arrive with life
    t = Clamp01(t);
    constexpr float c1 = 1.55f, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3.0f) + c1 * std::pow(t - 1, 2.0f);
}
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
// A smooth arc from p to q through a control point (a hand moving a mouse never goes in a straight line).
D2D1_POINT_2F Arc(D2D1_POINT_2F p, D2D1_POINT_2F c, D2D1_POINT_2F q, float t) {
    const float u = 1 - t;
    return {u * u * p.x + 2 * u * t * c.x + t * t * q.x, u * u * p.y + 2 * u * t * c.y + t * t * q.y};
}
D2D1_RECT_F Inflate(D2D1_RECT_F r, float dx, float dy) { return D2D1::RectF(r.left - dx, r.top - dy, r.right + dx, r.bottom + dy); }
D2D1_RECT_F Shift(D2D1_RECT_F r, float dx, float dy) { return D2D1::RectF(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy); }
D2D1_RECT_F Centered(D2D1_POINT_2F c, float w, float h) { return D2D1::RectF(c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2); }

std::wstring FormatBytes(uint64_t bytes) {
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024 * 1024);
    wchar_t buf[32];
    if (gb >= 0.95) swprintf_s(buf, L"%.1f GB", gb);
    else swprintf_s(buf, L"%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024));
    return buf;
}

// The live line while SoluSetup runs: its stage, and a percent once a step reports real byte progress.
std::wstring SoluSetupStatusLine(const SoluSetupProgress& p) {
    std::wstring line = p.label.empty() ? L"Setting up Solu…" : p.label;
    if (p.totalBytes > 0) {
        const int percent = static_cast<int>(std::min<uint64_t>(100, p.doneBytes * 100 / p.totalBytes));
        line += L"\n" + FormatBytes(p.doneBytes) + L" of " + FormatBytes(p.totalBytes) + L" · " + std::to_wstring(percent) + L"%";
    }
    return line;
}

// ---- colour ------------------------------------------------------------------------------------------------------
constexpr D2D1_COLOR_F kBrand = Rgba(0.345f, 0.396f, 0.949f, 1);      // #5865F2
constexpr D2D1_COLOR_F kBrandLight = Rgba(0.486f, 0.525f, 1.0f, 1);   // #7C86FF
constexpr D2D1_COLOR_F kBrandDeep = Rgba(0.227f, 0.259f, 0.847f, 1);  // #3A42D8
constexpr D2D1_COLOR_F kButtonBlue = Rgba(0.04f, 0.47f, 1.0f, 1);
constexpr D2D1_COLOR_F kInk = Rgba(0.030f, 0.030f, 0.040f, 1);       // the little island in the scenes

// ---- words -------------------------------------------------------------------------------------------------------
struct Words {
    const wchar_t* title;
    const wchar_t* body;
};
const Words kWords[kTourSteps] = {
    {L"This is Eflow", L"It lives at the top of your screen. Scroll or middle-click on it to open it, and move away to tuck it "
                       L"back. Your clicks go through to whatever is underneath."},
    {L"What matters, at a glance", L"Closed, the island shows what is happening right now: the song you're playing, your AI "
                                   L"agents at work, a login code. Middle-click it to see more."},
    {L"Notifications, in one calm place", L"Messages slide into the island, then step aside. Mute an app and it stays quiet "
                                          L"everywhere. Your notifications never leave your PC."},
    {L"Approve your AI agents", L"When Claude Code or Codex needs your OK, it asks right here. One click to approve, and nothing "
                                L"is ever approved on its own."},
    {L"Drop it. Keep it. Send it.", L"Drag files onto the island to keep them on the Shelf, send them in an encrypted SafeShare "
                                    L"room, or open them on your phone over your own Wi-Fi."},
    {L"Meet Solu", L"Say “Solu” or press Ctrl+Alt+Space, then just ask: a reminder, a song, a quick answer. Solu runs "
                   L"on your PC."},
    {L"You're all set", L"A few shortcuts for later. You can take this tour again from the Eflow icon next to the clock."},
};

}  // namespace

// ---- state ---------------------------------------------------------------------------------------------------------

void IslandWindow::StartTour(TourStep startAt) {
    if (tourOn_) {                                 // already running: "Set up Solu" on the tab just jumps to that step
        if (startAt != TourStep::Hello && expanded_) TourGo(static_cast<int>(startAt));
        return;
    }
    tourOn_ = true;
    tourStep_ = static_cast<int>(TourStep::Invite);
    tourPrev_ = -1;
    tourSeen_ = 0;
    tourTime_ = tourPrevTime_ = tourClock_ = 0;
    tourSwap_ = 1;
    tourAskedNotifications_ = tourAskedPhone_ = tourAskedAgents_ = tourAskedSolu_ = false;
    std::fill(std::begin(tourDoneCheered_), std::end(tourDoneCheered_), false);
    tourFinishing_ = false;
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    reducedMotion_ = !animations;
    Log("tour: started%s", reducedMotion_ ? " (still scenes: animations are off in Windows)" : "");
    if (expanded_ || startAt != TourStep::Hello) {   // already open, or asked to open straight on a step
        tourStep_ = tourSeen_ = std::max(static_cast<int>(startAt), static_cast<int>(TourStep::Hello));
        TourEnter(static_cast<TourStep>(tourStep_));
        if (!expanded_) Expand();
        ApplyHeightTarget();
    } else {
        RefreshCollapsed();                       // the closed island becomes the invitation
    }
    Animate();
}

void IslandWindow::SetSoluSetup(bool detected, const SoluSetupPlan& plan, const SoluSetupProgress& progress) {
    soluPlanReady_ = detected;
    soluPlan_ = plan;
    soluSetupProgress_ = progress;
    if (tourOn_ && tourStep_ == static_cast<int>(TourStep::Solu)) Animate();
}

void IslandWindow::StopTour() {
    if (!tourOn_) return;
    KillTimer(hwnd_, kTourTimer);
    if (tourStep_ > static_cast<int>(TourStep::Invite)) TourLeave(static_cast<TourStep>(tourStep_));
    tourOn_ = false;
    tourFinishing_ = false;
    if (expanded_) Collapse();
    else RefreshCollapsed();
    Animate();
}

void IslandWindow::SetTourStatus(const TourStatus& status) {
    tourStatus_ = status;
    if (tourOn_) Animate();
}

bool IslandWindow::TourShown() const {
    return tourOn_ && expanded_ && tourStep_ > static_cast<int>(TourStep::Invite) && permissions_.empty() && reminders_.ringing.empty() &&
           !dragOver_;
}

void IslandWindow::TourGo(int step) {
    step = std::clamp(step, static_cast<int>(TourStep::Hello), static_cast<int>(TourStep::Done));
    if (step == tourStep_) return;
    TourLeave(static_cast<TourStep>(tourStep_));
    tourDir_ = step > tourStep_ ? 1 : -1;
    tourPrev_ = tourStep_;
    tourPrevTime_ = tourTime_;
    tourStep_ = step;
    tourTime_ = 0;
    tourSwap_ = reducedMotion_ ? 1.0f : 0.0f;
    tourSeen_ = std::max(tourSeen_, step);
    hover_ = -1;
    TourEnter(static_cast<TourStep>(step));
    Animate();
}

void IslandWindow::TourEnter(TourStep step) {
    if (!mascot_) return;
    if (step == TourStep::Notifications) mascot_->Notify();             // the ear badge pops: a notification
    if (step == TourStep::Agents) mascot_->SetPermissionPending(true);  // the "?" above its head: something needs you
    if (step == TourStep::Done) mascot_->Cheer();
}

void IslandWindow::TourLeave(TourStep step) {
    if (!mascot_) return;
    if (step == TourStep::Notifications) mascot_->ClearBadge();
    if (step == TourStep::Agents) mascot_->SetPermissionPending(!permissions_.empty());
}

void IslandWindow::TourFinish(bool completed) {
    if (completed) {                              // a happy hop, then the island tucks itself away
        tourFinishing_ = true;
        if (mascot_) mascot_->Cheer();
        SetTimer(hwnd_, kTourTimer, 1100, nullptr);
        Log("tour: completed");
        return;
    }
    Log("tour: skipped at step %d", tourStep_);
    TourLeave(static_cast<TourStep>(tourStep_));
    tourOn_ = false;
    Collapse();
    if (tour_.finished) tour_.finished(false);
}

#ifndef NDEBUG
void IslandWindow::DebugTour(int step) {
    if (step < 0) {
        debugPinned_ = false;
        StopTour();
        return;
    }
    if (!tourOn_) StartTour();
    if (step == 0) {
        if (expanded_) Collapse();
        return;
    }
    debugPinned_ = true;
    if (!expanded_) Expand();
    if (tourStep_ != step) {
        TourGo(step);
        tourSwap_ = 1;                            // a snapshot shows the step, not the slide into it
    }
}
#endif

// What the step's buttons say and do, and its line of status: from what Windows and the tools report right now.
IslandWindow::TourAction IslandWindow::TourActions() const {
    TourAction a;
    a.primary = L"Next";
    switch (static_cast<TourStep>(tourStep_)) {
    case TourStep::Hello: a.primary = L"Let’s go"; break;
    case TourStep::Glance: break;
    case TourStep::Notifications:
        switch (notifications_.access) {
        case NotificationAccess::Allowed: a.status = L"Notifications are on"; a.statusKind = 1; break;
        case NotificationAccess::Denied:
            a.status = L"Turned off in Windows settings";
            a.statusKind = 3;
            a.secondary = L"Open settings";
            a.secondaryOpensSettings = true;
            break;
        case NotificationAccess::Unsupported: a.status = L"Needs the installed version of Eflow"; a.statusKind = 3; break;
        default:
            if (tourAskedNotifications_) {
                a.status = L"Choose Allow when Windows asks";
                a.statusKind = 2;
            } else {
                a.primary = L"Allow notifications";
                a.primaryAdvances = false;
                a.secondary = L"Not now";
            }
        }
        break;
    case TourStep::Agents: {
        const bool claude = tourStatus_.claudeAvailable, codex = tourStatus_.codexAvailable;
        const bool claudeOk = !claude || agents_.claudeHooked, codexOk = !codex || agents_.codexHooked;
        if (!claude && !codex) {
            a.status = L"Claude Code and Codex aren’t on this PC";
        } else if (claudeOk && codexOk) {
            a.status = claude && codex ? L"Claude Code and Codex are connected" : claude ? L"Claude Code is connected" : L"Codex is connected";
            a.statusKind = 1;
        } else if (tourAskedAgents_) {
            a.status = L"Connecting…";
            a.statusKind = 2;
        } else {
            const bool both = !claudeOk && !codexOk;
            a.primary = both ? L"Connect both" : !claudeOk ? L"Connect Claude Code" : L"Connect Codex";
            a.primaryAdvances = false;
            a.secondary = L"Not now";
        }
        break;
    }
    case TourStep::Files:
        switch (tourStatus_.firewall) {
        case FirewallAccess::Allowed: a.status = L"Your phone can connect"; a.statusKind = 1; break;
        case FirewallAccess::Blocked:
            a.status = L"Windows Firewall blocks your phone";
            a.statusKind = 3;
            a.secondary = L"Open settings";
            a.secondaryOpensSettings = true;
            break;
        default:
            if (tourAskedPhone_) {
                a.status = L"Choose Allow when Windows asks";
                a.statusKind = 2;
            } else {
                a.primary = L"Allow phone connections";
                a.primaryAdvances = false;
                a.secondary = L"Not now";
            }
        }
        break;
    case TourStep::Solu:
        if (!soluPlanReady_) {
            a.status = L"Checking this PC…";
            a.statusKind = 2;
        } else if (soluSetupProgress_.stage != SoluSetupStage::Idle && soluSetupProgress_.stage != SoluSetupStage::Done &&
                  soluSetupProgress_.stage != SoluSetupStage::Failed) {
            a.status = SoluSetupStatusLine(soluSetupProgress_);
            a.statusKind = 2;
        } else if (soluSetupProgress_.stage == SoluSetupStage::Failed) {
            a.status = soluSetupProgress_.error.empty() ? L"Setting up Solu didn’t finish" : soluSetupProgress_.error;
            a.statusKind = 3;
            a.primary = L"Try again";
            a.primaryAdvances = false;
            a.secondary = L"Not now";
        } else if (tourStatus_.micBlocked) {
            a.status = L"Microphone access is off in Windows";
            a.statusKind = 3;
            a.secondary = L"Open settings";
            a.secondaryOpensSettings = true;
        } else if (solu_.enabled && solu_.state == SoluState::Loading) {
            a.status = L"Waking Solu up…";
            a.statusKind = 2;
        } else if (solu_.enabled) {
            a.status = L"Solu is listening for its name";
            a.statusKind = 1;
        } else if (tourAskedSolu_) {
            a.status = L"Waking Solu up…";
            a.statusKind = 2;
        } else if (!soluPlan_.NothingToDo()) {
            const SoluModelChoice& model = soluPlan_.models[static_cast<size_t>(soluPlan_.recommended)];
            a.status = L"Recommended for this PC: " + model.label + L"\nAbout " + FormatBytes(soluPlan_.EstimateBytes(model.id)) + L" to download";
            a.primary = L"Set up Solu";
            a.primaryAdvances = false;
            a.secondary = L"Not now";
        } else {
            a.primary = L"Turn on Solu";
            a.primaryAdvances = false;
            a.secondary = L"Not now";
        }
        break;
    case TourStep::Done: a.primary = L"Start using Eflow"; break;
    default: break;
    }
    return a;
}

IslandWindow::TourLayout IslandWindow::TourGeometry(const D2D1_RECT_F& island) const {
    TourLayout g;
    const float left = island.left, top = island.top;
    g.header = D2D1::RectF(left + 22, top + 14, left + kExpandedW - 22, top + 40);
    IDWriteTextFormat* skipFont = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float skipW = std::ceil(gfx_.MeasureWidth(L"Skip tour", skipFont));
    g.skip = D2D1::RectF(g.header.right - skipW - 8, g.header.top, g.header.right + 4, g.header.bottom);
    float x = g.skip.left - 18;                                         // the dots, right to left before "Skip tour"
    for (int i = kTourSteps - 1; i >= 0; --i) {
        const float w = i + 1 == tourStep_ ? 18.0f : 6.0f;
        g.dots[static_cast<size_t>(i)] = D2D1::RectF(x - w, (g.header.top + g.header.bottom) / 2 - 3, x, (g.header.top + g.header.bottom) / 2 + 3);
        x -= w + 6;
    }
    g.stage = D2D1::RectF(left + 20, top + 52, left + 20 + 318, island.bottom - 20);
    g.text = D2D1::RectF(g.stage.right + 26, g.stage.top + 6, left + kExpandedW - (mascot_ ? kMascotLane - 6 : 26.0f), g.stage.bottom);

    // Buttons along the bottom of the text: primary at the right, the quiet one before it, "Back" at the left.
    const TourAction act = TourActions();
    IDWriteTextFormat* button = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float h = 32, y1 = g.text.bottom, y0 = y1 - h;
    const float pw = std::ceil(gfx_.MeasureWidth(act.primary.c_str(), button)) + 34;
    g.primary = D2D1::RectF(g.text.right - pw, y0, g.text.right, y1);
    if (!act.secondary.empty()) {
        const float sw = std::ceil(gfx_.MeasureWidth(act.secondary.c_str(), button)) + 28;
        g.secondary = D2D1::RectF(g.primary.left - 8 - sw, y0, g.primary.left - 8, y1);
    }
    if (tourStep_ > static_cast<int>(TourStep::Hello)) {
        const float bw = std::ceil(gfx_.MeasureWidth(L"Back", button)) + 12;
        g.back = D2D1::RectF(g.text.left - 6, y0, g.text.left - 6 + bw, y1);
        const float limit = (act.secondary.empty() ? g.primary.left : g.secondary.left) - 6;
        if (g.back.right > limit) g.back = {};                            // no room: the dots still go back
    }
    if (tourStep_ == static_cast<int>(TourStep::Done)) g.toggle = D2D1::RectF(g.text.left, y0 - 48, g.text.right, y0 - 14);
    return g;
}

int IslandWindow::TourHit(D2D1_POINT_2F p, const D2D1_RECT_F& island) const {
    if (tourFinishing_) return -1;
    const TourLayout g = TourGeometry(island);
    auto in = [&](const D2D1_RECT_F& r, float slack = 0) {
        return r.right > r.left && p.x >= r.left - slack && p.x <= r.right + slack && p.y >= r.top - slack && p.y <= r.bottom + slack;
    };
    if (in(g.primary)) return kHitTourPrimary;
    if (in(g.secondary)) return kHitTourSecondary;
    if (in(g.back)) return kHitTourBack;
    if (in(g.skip)) return kHitTourSkip;
    if (in(g.toggle)) return kHitTourSwitch;
    for (int i = 0; i < kTourSteps; ++i)
        if (i + 1 != tourStep_ && i + 1 <= tourSeen_ && in(g.dots[static_cast<size_t>(i)], 5)) return kHitTourDot + i;
    return -1;
}

void IslandWindow::TourClick(int id) {
    const TourAction act = TourActions();
    const auto step = static_cast<TourStep>(tourStep_);
    if (id == kHitTourSkip) return TourFinish(false);
    if (id == kHitTourBack) return TourGo(tourStep_ - 1);
    if (id >= kHitTourDot && id < kHitTourDot + kTourSteps) return TourGo(id - kHitTourDot + 1);
    if (id == kHitTourSwitch) {
        tourStatus_.autostart = !tourStatus_.autostart;               // shown at once; the app confirms on its next refresh
        if (tour_.autostart) tour_.autostart(tourStatus_.autostart);
        return;
    }
    if (id == kHitTourSecondary) {
        if (!act.secondaryOpensSettings) return TourGo(tourStep_ + 1);
        if (!tour_.openSettings) return;
        tour_.openSettings(step == TourStep::Notifications ? L"ms-settings:privacy-notifications"
                           : step == TourStep::Files       ? L"windowsdefender://network/"
                                                           : L"ms-settings:privacy-microphone");
        return;
    }
    if (id != kHitTourPrimary) return;
    if (act.primaryAdvances) {
        if (step == TourStep::Done) return TourFinish(true);
        return TourGo(tourStep_ + 1);
    }
    switch (step) {                               // the step's own thing: Windows (or the tool) answers in a moment
    case TourStep::Notifications:
        tourAskedNotifications_ = true;
        if (tour_.allowNotifications) tour_.allowNotifications();
        break;
    case TourStep::Agents:
        tourAskedAgents_ = true;
        if (tour_.connectAgents) tour_.connectAgents();
        break;
    case TourStep::Files:
        tourAskedPhone_ = true;
        if (tour_.allowPhone) tour_.allowPhone();
        break;
    case TourStep::Solu:
        tourAskedSolu_ = true;
        if (soluPlanReady_ && !soluPlan_.NothingToDo()) {
            if (tour_.soluSetupBegin) tour_.soluSetupBegin(soluPlan_.models[static_cast<size_t>(soluPlan_.recommended)].id);
        } else if (tour_.turnOnSolu) {
            tour_.turnOnSolu();
        }
        break;
    default: break;
    }
}

bool IslandWindow::TourAnimate(float dt) {
    if (!tourOn_) return false;
    bool moving = false;
    tourClock_ += dt;
    if (expanded_) {
        tourTime_ += dt;
        tourPrevTime_ += dt;
        moving |= Approach(tourSwap_, 1.0f, dt, 7.5f);
    }
    for (int i = 0; i < 5; ++i) moving |= Approach(tourHover_[static_cast<size_t>(i)], hover_ == kHitTourPrimary + i ? 1.0f : 0.0f, dt, 22.0f);
    // A permission the user just gave: the cat is pleased (once per step).
    const bool asked = (tourStep_ == static_cast<int>(TourStep::Notifications) && tourAskedNotifications_) ||
                       (tourStep_ == static_cast<int>(TourStep::Agents) && tourAskedAgents_) ||
                       (tourStep_ == static_cast<int>(TourStep::Files) && tourAskedPhone_) ||
                       (tourStep_ == static_cast<int>(TourStep::Solu) && tourAskedSolu_);
    if (asked && !tourDoneCheered_[tourStep_] && TourActions().statusKind == 1) {
        tourDoneCheered_[tourStep_] = true;
        if (mascot_) mascot_->Cheer();
        Log("tour: step %d done", tourStep_);
    }
    return moving;
}

// ---- drawing: the frame ---------------------------------------------------------------------------------------------

void IslandWindow::DrawTour(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha) {
    const TourLayout g = TourGeometry(base);
    const float midY = (g.header.top + g.header.bottom) / 2;

    // Header: the mark, where we are, the steps as dots, and a way out.
    DrawLogoMark(dc, D2D1::Point2F(g.header.left + 9, midY), 18, alpha, true);
    DrawText(dc, L"Getting started", gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             D2D1::RectF(g.header.left + 26, g.header.top, g.header.left + 220, g.header.bottom), WithAlpha(kText, 0.80f * alpha));
    for (int i = 0; i < kTourSteps; ++i) {
        const D2D1_RECT_F& d = g.dots[static_cast<size_t>(i)];
        const bool current = i + 1 == tourStep_, seen = i + 1 <= tourSeen_;
        const bool hot = hover_ == kHitTourDot + i;
        const D2D1_COLOR_F c = current ? kBrandLight : D2D1::ColorF(1, 1, 1, seen ? (hot ? 0.75f : 0.42f) : 0.16f);
        brush_->SetColor(WithAlpha(c, alpha));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{d, 3, 3}, brush_.Get());
    }
    DrawText(dc, L"Skip tour", gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), g.skip,
             WithAlpha(kText, (0.46f + 0.44f * tourHover_[3]) * alpha), DWRITE_TEXT_ALIGNMENT_TRAILING);

    // The stage: the scene of the step, sliding over from the one before.
    const D2D1_ROUNDED_RECT card{g.stage, 16, 16};
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.035f * alpha));
    dc->FillRoundedRectangle(card, brush_.Get());
    Com<ID2D1RoundedRectangleGeometry> mask;
    gfx_.d2dFactory()->CreateRoundedRectangleGeometry(card, &mask);
    const float e = EaseOut(tourSwap_);
    const bool sliding = tourSwap_ < 1.0f && tourPrev_ > 0;
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    if (mask) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), mask.Get()), nullptr);
    if (sliding) {
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-tourDir_ * 36.0f * e, 0) * old);
        DrawTourScene(dc, static_cast<TourStep>(tourPrev_), g.stage, alpha * (1 - e), tourPrevTime_);
    }
    dc->SetTransform(D2D1::Matrix3x2F::Translation(sliding ? tourDir_ * 36.0f * (1 - e) : 0.0f, 0) * old);
    DrawTourScene(dc, static_cast<TourStep>(tourStep_), g.stage, alpha * (sliding ? e : 1.0f), tourTime_);
    dc->SetTransform(old);
    if (mask) dc->PopLayer();
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.07f * alpha));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{Inflate(g.stage, -0.5f, -0.5f), 15.5f, 15.5f}, brush_.Get(), 1.0f);

    // The words, sliding the same way.
    if (sliding) DrawTourStep(dc, static_cast<TourStep>(tourPrev_), g, alpha * (1 - e), -tourDir_ * 18.0f * e, tourPrevTime_);
    DrawTourStep(dc, static_cast<TourStep>(tourStep_), g, alpha * (sliding ? e : 1.0f), sliding ? tourDir_ * 18.0f * (1 - e) : 0.0f, tourTime_);
}

void IslandWindow::DrawTourStep(ID2D1DeviceContext* dc, TourStep step, const TourLayout& g, float a, float dx, float time) {
    if (a < 0.003f) return;
    const Words& w = kWords[static_cast<int>(step) - 1];
    const float width = g.text.right - g.text.left;
    IDWriteTextFormat* titleFont = gfx_.Wrapped(17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float titleH = gfx_.MeasureHeight(w.title, titleFont, width);
    IDWriteTextFormat* bodyFont = gfx_.Wrapped(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    const float bodyH = gfx_.MeasureHeight(w.body, bodyFont, width);
    const float x0 = g.text.left + dx, x1 = g.text.right + dx;
    DrawText(dc, w.title, gfx_.Wrapped(17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x0, g.text.top, x1, g.text.top + titleH + 2),
             WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    const float bodyTop = g.text.top + titleH + 7;
    DrawText(dc, w.body, gfx_.Wrapped(12.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(x0, bodyTop, x1, bodyTop + bodyH + 2),
             WithAlpha(kText, 0.64f * a), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (static_cast<int>(step) != tourStep_) return;              // (the step sliding away shows only its words)

    const TourAction act = TourActions();
    if (!act.status.empty()) {                                     // the step's state: on, waiting, or off (and why)
        const float y = bodyTop + bodyH + 12;
        const D2D1_COLOR_F c = act.statusKind == 1 ? kGreen : act.statusKind == 3 ? kAmber : WithAlpha(kText, 0.62f);
        const D2D1_POINT_2F icon{x0 + 7, y + 9};
        if (act.statusKind == 2) {
            for (int i = 0; i < 8; ++i) {                          // a small spinner: dots chasing round
                const float ang = i * kPi / 4 + time * 5.0f;
                const float fade = std::fmod(i / 8.0f + time * 0.8f, 1.0f);
                brush_->SetColor(WithAlpha(kText, (0.15f + 0.75f * fade) * a));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(icon.x + std::cos(ang) * 5.0f, icon.y + std::sin(ang) * 5.0f), 1.3f, 1.3f), brush_.Get());
            }
        } else if (act.statusKind == 1 || act.statusKind == 3) {
            DrawText(dc, act.statusKind == 1 ? L"" : L"", gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL, true), Centered(icon, 18, 18),
                     WithAlpha(c, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            brush_->SetColor(WithAlpha(kText, 0.35f * a));
            dc->FillEllipse(D2D1::Ellipse(icon, 2.5f, 2.5f), brush_.Get());
        }
        // (wrapped: Solu's setup lines name the model and its size, more than one line holds)
        IDWriteTextFormat* statusFont = gfx_.Wrapped(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float statusH = std::max(18.0f, gfx_.MeasureHeight(act.status.c_str(), statusFont, x1 - x0 - 20));
        DrawText(dc, act.status.c_str(), statusFont, D2D1::RectF(x0 + 20, y + 1, x1, y + 1 + statusH), WithAlpha(c, a),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        // Solu's setup, while it downloads: a real progress bar under the line.
        const SoluSetupStage st = soluSetupProgress_.stage;
        if (step == TourStep::Solu && st != SoluSetupStage::Idle && st != SoluSetupStage::Done && st != SoluSetupStage::Failed) {
            const D2D1_RECT_F track = D2D1::RectF(x0 + 20, y + statusH + 8, x1, y + statusH + 12);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{track, 2, 2}, brush_.Get());
            brush_->SetColor(WithAlpha(kAccent, a));
            if (soluSetupProgress_.totalBytes > 0) {
                const float f = std::min(1.0f, static_cast<float>(soluSetupProgress_.doneBytes) / static_cast<float>(soluSetupProgress_.totalBytes));
                dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(track.left, track.top, Lerp(track.left, track.right, f), track.bottom), 2, 2}, brush_.Get());
            } else {                                               // size not known yet: a segment sweeping across
                const float seg = (track.right - track.left) * 0.28f;
                const float x = track.left + std::fmod(time * 0.6f, 1.0f) * (track.right - track.left + seg) - seg;
                dc->PushAxisAlignedClip(track, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, track.top, x + seg, track.bottom), 2, 2}, brush_.Get());
                dc->PopAxisAlignedClip();
            }
        }
    }
    if (step == TourStep::Done) {                                  // "Start Eflow with Windows", with the same switch as Settings
        const D2D1_RECT_F row = Shift(g.toggle, dx, 0);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.05f + 0.04f * tourHover_[4]) * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{row, 10, 10}, brush_.Get());
        DrawText(dc, L"Start Eflow with Windows", gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                 D2D1::RectF(row.left + 12, row.top, row.right - 56, row.bottom), WithAlpha(kText, a));
        const float cy = (row.top + row.bottom) / 2;
        const D2D1_RECT_F sw = D2D1::RectF(row.right - 50, cy - 11, row.right - 10, cy + 11);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.20f * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{sw, 11, 11}, brush_.Get());
        if (tourStatus_.autostart) {
            brush_->SetColor(D2D1::ColorF(0.20f, 0.78f, 0.35f, a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{sw, 11, 11}, brush_.Get());
        }
        const D2D1_POINT_2F knob{tourStatus_.autostart ? sw.right - 11.0f : sw.left + 11.0f, cy};
        brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.28f * a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob.x, knob.y + 1.2f), 9.0f, 9.0f), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, a));
        dc->FillEllipse(D2D1::Ellipse(knob, 9.0f, 9.0f), brush_.Get());
    }
    if (g.back.right > g.back.left)
        DrawText(dc, L"Back", gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), Shift(g.back, dx, 0),
                 WithAlpha(kText, (0.50f + 0.45f * tourHover_[2]) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!act.secondary.empty()) DrawShareButton(dc, Shift(g.secondary, dx, 0), act.secondary.c_str(), false, true, hover_ == kHitTourSecondary, a);
    if (!tourFinishing_) DrawShareButton(dc, Shift(g.primary, dx, 0), act.primary.c_str(), true, true, hover_ == kHitTourPrimary, a);
}

// The closed island, inviting: the mark, a welcome, how to open it, and a soft pulse so it is noticed.
float IslandWindow::TourInviteW() const {
    const float w1 = std::ceil(gfx_.MeasureWidth(L"Welcome to Eflow", gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)));
    const float w2 = std::ceil(gfx_.MeasureWidth(L"Scroll or middle-click to open", gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL)));
    return 40 + w1 + 16 + w2 + 38;
}

void IslandWindow::DrawTourInvite(ID2D1DeviceContext* dc, const D2D1_RECT_F& pill, float alpha) {
    if (alpha < 0.003f) return;
    const float t = reducedMotion_ ? 0.6f : tourClock_;
    const float pulse = 0.5f + 0.5f * std::sin(t * 2 * kPi / 2.4f);
    const float radius = (pill.bottom - pill.top) / 2;
    for (int k = 1; k <= 3; ++k) {                                 // the glow, breathing outwards
        const float grow = k * 1.9f + pulse * 1.2f;
        brush_->SetColor(WithAlpha(kBrandLight, (0.34f - 0.09f * k) * (0.45f + 0.55f * pulse) * alpha));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{Inflate(pill, grow, grow), radius + grow, radius + grow}, brush_.Get(), 1.4f);
    }
    const float cy = (pill.top + pill.bottom) / 2;
    DrawLogoMark(dc, D2D1::Point2F(pill.left + 20, cy), 20 * (1 + 0.035f * pulse), alpha, true);
    IDWriteTextFormat* strong = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float w1 = std::ceil(gfx_.MeasureWidth(L"Welcome to Eflow", strong));
    DrawText(dc, L"Welcome to Eflow", strong, D2D1::RectF(pill.left + 38, pill.top, pill.left + 38 + w1 + 2, pill.bottom), WithAlpha(kText, alpha));
    DrawText(dc, L"Scroll or middle-click to open", gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
             D2D1::RectF(pill.left + 38 + w1 + 14, pill.top, pill.right - 34, pill.bottom), WithAlpha(kText, 0.58f * alpha));
    // A little mouse at the end, its wheel turning.
    const D2D1_POINT_2F m{pill.right - 19, cy};
    const D2D1_RECT_F body = Centered(m, 11, 16);
    brush_->SetColor(WithAlpha(kText, 0.80f * alpha));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{body, 5.5f, 5.5f}, brush_.Get(), 1.2f);
    const float roll = reducedMotion_ ? 0.3f : std::fmod(t * 1.6f, 1.0f);
    brush_->SetColor(WithAlpha(kBrandLight, (1 - roll) * alpha));
    dc->DrawLine(D2D1::Point2F(m.x, body.top + 3.0f + roll * 4.0f), D2D1::Point2F(m.x, body.top + 5.5f + roll * 4.0f), brush_.Get(), 1.6f,
                 TourStroke());
}

// ---- drawing: shapes ------------------------------------------------------------------------------------------------

ID2D1StrokeStyle* IslandWindow::TourStroke() {
    if (!tourStroke_) {
        const auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        gfx_.d2dFactory()->CreateStrokeStyle(props, nullptr, 0, &tourStroke_);
    }
    return tourStroke_.Get();
}

ID2D1PathGeometry* IslandWindow::TourPath(int which) {
    Com<ID2D1PathGeometry>& slot = tourPaths_[static_cast<size_t>(which)];
    if (slot) return slot.Get();
    Com<ID2D1GeometrySink> sink;
    if (FAILED(gfx_.d2dFactory()->CreatePathGeometry(&slot)) || FAILED(slot->Open(&sink))) return nullptr;
    auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts) {
        auto it = pts.begin();
        sink->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
        for (++it; it != pts.end(); ++it) sink->AddLine(*it);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    };
    using D2D1::Point2F;
    switch (which) {
    case 0:                                                        // the pointer, tip at the origin
        poly({Point2F(0, 0), Point2F(0, 17), Point2F(4.2f, 13.2f), Point2F(7.2f, 19.6f), Point2F(9.9f, 18.4f), Point2F(7.0f, 12.2f),
              Point2F(12.4f, 12.2f)});
        break;
    case 1:                                                        // the mark's ears (in its 512-unit square)
        poly({Point2F(138, 250), Point2F(166, 130), Point2F(230, 214)});
        poly({Point2F(374, 250), Point2F(346, 130), Point2F(282, 214)});
        break;
    case 2:                                                        // ... its closed, smiling eyes
        for (float x : {176.0f, 280.0f}) {
            sink->BeginFigure(Point2F(x, 300), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(x + 28, 266), Point2F(x + 56, 300)));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        break;
    case 3:                                                        // a document with a folded corner, 20 x 26
        sink->BeginFigure(Point2F(2, 0), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(Point2F(13, 0));
        sink->AddLine(Point2F(20, 7));
        sink->AddLine(Point2F(20, 24));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(20, 26), Point2F(18, 26)));
        sink->AddLine(Point2F(2, 26));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(0, 26), Point2F(0, 24)));
        sink->AddLine(Point2F(0, 2));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(0, 0), Point2F(2, 0)));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        break;
    default: break;
    }
    sink->Close();
    return slot.Get();
}

// Eflow's mark: the island as a happy face (assets/branding/eflow-icon.svg), optionally on its blurple tile.
void IslandWindow::DrawLogoMark(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float size, float a, bool tile) {
    const float s = size / 512.0f;
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(center.x - size / 2, center.y - size / 2) * old);
    if (tile) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, kBrandLight}, {1.0f, kBrandDeep}};
        Com<ID2D1GradientStopCollection> coll;
        Com<ID2D1LinearGradientBrush> fill;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &coll)) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(512, 512)), coll.Get(), &fill))) {
            fill->SetOpacity(a);
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(0, 0, 512, 512), 116, 116}, fill.Get());
        }
    }
    brush_->SetColor(D2D1::ColorF(1, 1, 1, a));
    if (ID2D1PathGeometry* ears = TourPath(1)) {
        dc->FillGeometry(ears, brush_.Get());
        dc->DrawGeometry(ears, brush_.Get(), 36, TourStroke());
    }
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(92, 196, 420, 380), 92, 92}, brush_.Get());
    brush_->SetColor(WithAlpha(tile ? kBrandDeep : kInk, a));
    if (ID2D1PathGeometry* eyes = TourPath(2)) dc->DrawGeometry(eyes, brush_.Get(), 24, TourStroke());
    dc->SetTransform(old);
}

void IslandWindow::DrawCursor(ID2D1DeviceContext* dc, D2D1_POINT_2F tip, float a, float press) {
    ID2D1PathGeometry* arrow = TourPath(0);
    if (!arrow || a < 0.003f) return;
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const float k = 1.0f - 0.12f * Clamp01(press);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(tip.x + 1.0f, tip.y + 1.8f) * old);
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.32f * a));               // its shadow
    dc->FillGeometry(arrow, brush_.Get());
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(tip.x, tip.y) * old);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, a));
    dc->FillGeometry(arrow, brush_.Get());
    brush_->SetColor(WithAlpha(kInk, a));
    dc->DrawGeometry(arrow, brush_.Get(), 1.1f, TourStroke());
    dc->SetTransform(old);
}

void IslandWindow::TourRipple(ID2D1DeviceContext* dc, D2D1_POINT_2F at, float phase, float a) {
    if (phase <= 0 || phase >= 1) return;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.75f * (1 - phase) * a));
    dc->DrawEllipse(D2D1::Ellipse(at, 4 + 14 * EaseOut(phase), 4 + 14 * EaseOut(phase)), brush_.Get(), 1.6f);
}

void IslandWindow::TourMiniIsland(ID2D1DeviceContext* dc, D2D1_RECT_F r, float a) {
    const float radius = std::min(14.0f, (r.bottom - r.top) / 2);
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.30f * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{Shift(r, 0, 2.5f), radius, radius}, brush_.Get());
    brush_->SetColor(WithAlpha(kInk, a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{r, radius, radius}, brush_.Get());
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.11f * a));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{Inflate(r, -0.5f, -0.5f), std::max(0.0f, radius - 0.5f), std::max(0.0f, radius - 0.5f)},
                             brush_.Get(), 1.0f);
}

// A little desktop for the scenes: a wallpaper, two windows and a taskbar.
void IslandWindow::TourDesktop(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, bool windows) {
    const float w = s.right - s.left, h = s.bottom - s.top;
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, Rgba(0.30f, 0.24f, 0.72f, 1)}, {0.55f, Rgba(0.13f, 0.12f, 0.36f, 1)}, {1.0f, Rgba(0.055f, 0.06f, 0.16f, 1)}};
    Com<ID2D1GradientStopCollection> coll;
    Com<ID2D1RadialGradientBrush> wall;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &coll)) &&
        SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(s.left + w * 0.72f, s.bottom + h * 0.15f),
                                                                                     D2D1::Point2F(0, 0), w * 0.95f, h * 1.25f),
                                                coll.Get(), &wall))) {
        wall->SetOpacity(a);
        dc->FillRectangle(s, wall.Get());
    }
    if (windows) {
        for (const D2D1_RECT_F& win : {D2D1::RectF(s.left + 22, s.top + 70, s.left + 168, s.top + 170),
                                       D2D1::RectF(s.right - 132, s.top + 92, s.right - 26, s.top + 182)}) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.055f * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{win, 7, 7}, brush_.Get());
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.045f * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(win.left, win.top, win.right, win.top + 12), 7, 7}, brush_.Get());
        }
    }
    if (!windows) return;
    brush_->SetColor(D2D1::ColorF(0.03f, 0.03f, 0.08f, 0.55f * a));
    dc->FillRectangle(D2D1::RectF(s.left, s.bottom - 18, s.right, s.bottom), brush_.Get());
    for (int i = -2; i <= 2; ++i) {
        const D2D1_POINT_2F c{(s.left + s.right) / 2 + i * 15.0f, s.bottom - 9};
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (i == 0 ? 0.42f : 0.20f) * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{Centered(c, 9, 9), 2.5f, 2.5f}, brush_.Get());
    }
}

void IslandWindow::TourCaption(ID2D1DeviceContext* dc, D2D1_RECT_F s, const wchar_t* text, float a) {
    if (a < 0.003f) return;
    IDWriteTextFormat* fmt = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float w = std::ceil(gfx_.MeasureWidth(text, fmt)) + 22;
    const D2D1_RECT_F chip = Centered(D2D1::Point2F((s.left + s.right) / 2, s.bottom - 34 + (1 - a) * 4), w, 22);
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.50f * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{chip, 11, 11}, brush_.Get());
    DrawText(dc, text, fmt, chip, WithAlpha(kText, 0.94f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
}

void IslandWindow::TourKeycap(ID2D1DeviceContext* dc, D2D1_RECT_F r, const wchar_t* label, float press, float a) {
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.055f * a));             // the key's side, seen below its face
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{Shift(r, 0, 2.5f), 6, 6}, brush_.Get());
    const D2D1_RECT_F face = Shift(r, 0, 2.5f * press);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.11f + 0.13f * press) * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{face, 6, 6}, brush_.Get());
    if (press > 0.01f) {
        brush_->SetColor(WithAlpha(kBrandLight, 0.65f * press * a));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{Inflate(face, -0.5f, -0.5f), 5.5f, 5.5f}, brush_.Get(), 1.0f);
    }
    DrawText(dc, label, gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), face, WithAlpha(kText, (0.82f + 0.18f * press) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// A QR code to look at (the scenes never need it to scan): three finder squares and a fixed scatter of modules.
void IslandWindow::TourQr(ID2D1DeviceContext* dc, D2D1_RECT_F r, float a, bool light) {
    constexpr int n = 15;
    brush_->SetColor(light ? D2D1::ColorF(1, 1, 1, a) : D2D1::ColorF(0.95f, 0.95f, 0.97f, a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{r, 4, 4}, brush_.Get());
    const float m = (r.right - r.left - 8) / n;
    brush_->SetColor(D2D1::ColorF(0.06f, 0.06f, 0.09f, a));
    auto finder = [&](int fx, int fy, int x, int y) {
        const int dx = x - fx, dy = y - fy;
        if (dx < 0 || dy < 0 || dx > 6 || dy > 6) return -1;
        return (dx == 0 || dy == 0 || dx == 6 || dy == 6 || (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4)) ? 1 : 0;
    };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            int on = -1;
            for (auto [fx, fy] : {std::pair{0, 0}, std::pair{n - 7, 0}, std::pair{0, n - 7}})
                if (const int f = finder(fx, fy, x, y); f >= 0) on = f;
            if (on < 0) on = ((x * 73 + y * 151 + x * y * 7) % 11) < 5 ? 1 : 0;
            if (on) dc->FillRectangle(D2D1::RectF(r.left + 4 + x * m, r.top + 4 + y * m, r.left + 4 + (x + 1) * m + 0.3f, r.top + 4 + (y + 1) * m + 0.3f), brush_.Get());
        }
}

// ---- the scenes -----------------------------------------------------------------------------------------------------

void IslandWindow::DrawTourScene(ID2D1DeviceContext* dc, TourStep step, D2D1_RECT_F s, float a, float time) {
    if (a < 0.003f) return;
    switch (step) {
    case TourStep::Hello: return SceneHello(dc, s, a, time);
    case TourStep::Glance: return SceneGlance(dc, s, a, time);
    case TourStep::Notifications: return SceneNotifications(dc, s, a, time);
    case TourStep::Agents: return SceneAgents(dc, s, a, time);
    case TourStep::Files: return SceneFiles(dc, s, a, time);
    case TourStep::Solu: return SceneSolu(dc, s, a, time);
    case TourStep::Done: return SceneDone(dc, s, a, time);
    default: return;
    }
}

// The hand moves up to the island and turns the wheel; the island opens; the hand moves away and it closes again.
void IslandWindow::SceneHello(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time) {
    const float u = reducedMotion_ ? 2.6f : std::fmod(time, 5.4f);
    TourDesktop(dc, s, a, true);
    const float cx = (s.left + s.right) / 2, top = s.top + 10;
    const float open = EaseOutBack(Seg(u, 1.45f, 1.95f)) * (1 - EaseInOut(Seg(u, 3.75f, 4.2f)));
    const float w = Lerp(58, 214, open), h = std::max(6.0f, Lerp(6, 96, open));
    const D2D1_RECT_F r = D2D1::RectF(cx - w / 2, top, cx + w / 2, top + h);
    TourMiniIsland(dc, r, a);
    if (const float ca = Seg(open, 0.72f, 1.0f) * a; ca > 0.003f) {            // a tiny Music tab
        for (int i = 0; i < 5; ++i) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (i == 0 ? 0.24f : 0.08f) * ca));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(r.left + 12 + i * 30.0f, top + 10, r.left + 38 + i * 30.0f, top + 18), 4, 4}, brush_.Get());
        }
        brush_->SetColor(D2D1::ColorF(0.93f, 0.45f, 0.62f, ca));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(r.left + 12, top + 27, r.left + 40, top + 55), 6, 6}, brush_.Get());
        brush_->SetColor(D2D1::ColorF(1.0f, 0.78f, 0.45f, 0.55f * ca));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 31, top + 36), 6, 6), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.62f * ca));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(r.left + 50, top + 31, r.left + 150, top + 37), 3, 3}, brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.28f * ca));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(r.left + 50, top + 43, r.left + 118, top + 48), 2.5f, 2.5f}, brush_.Get());
        const D2D1_RECT_F track = D2D1::RectF(r.left + 12, top + 66, r.right - 12, top + 69);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * ca));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{track, 1.5f, 1.5f}, brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.82f * ca));
        const float f = 0.32f + 0.14f * Seg(u, 1.95f, 3.75f);
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(track.left, track.top, Lerp(track.left, track.right, f), track.bottom), 1.5f, 1.5f}, brush_.Get());
        for (int i = -1; i <= 1; ++i) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (i == 0 ? 0.88f : 0.55f) * ca));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + i * 26.0f, top + 82), i == 0 ? 5.5f : 3.5f, i == 0 ? 5.5f : 3.5f), brush_.Get());
        }
    }
    // The hand: up to the island, a little stillness while it is open, then away.
    const D2D1_POINT_2F pA{s.right - 48, s.bottom - 40}, pB{cx + 12, top + 3}, pC{s.left + 82, s.bottom - 54};
    auto hold = [&](float v) { return D2D1::Point2F(pB.x + std::sin((v - 1.25f) * 1.7f) * 3.0f, pB.y); };
    D2D1_POINT_2F p;
    if (u < 1.25f) p = Arc(pA, D2D1::Point2F(s.right - 36, top + 46), pB, EaseInOut(Seg(u, 0.25f, 1.25f)));
    else if (u < 3.35f) p = hold(u);
    else p = Arc(hold(3.35f), D2D1::Point2F(cx - 20, s.bottom - 36), pC, EaseInOut(Seg(u, 3.35f, 4.1f)));
    if (const float wa = Seg(u, 0.95f, 1.1f) * (1 - Seg(u, 1.6f, 1.8f)) * a; wa > 0.003f && !reducedMotion_) {   // the wheel turning
        const D2D1_POINT_2F m{p.x + 22, p.y + 24};
        const D2D1_RECT_F body = Centered(m, 11, 16);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.88f * wa));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{body, 5.5f, 5.5f}, brush_.Get(), 1.2f);
        const float roll = std::fmod(u * 3.2f, 1.0f);
        brush_->SetColor(WithAlpha(kBrandLight, (1 - roll) * wa));
        dc->DrawLine(D2D1::Point2F(m.x, body.top + 3 + roll * 4), D2D1::Point2F(m.x, body.top + 5.5f + roll * 4), brush_.Get(), 1.6f, TourStroke());
        const float ch = Seg(u, 0.95f, 1.1f) * (1 - Seg(u, 1.35f, 1.5f)) * a;
        for (int k = 0; k < 2; ++k) {                                  // ... and the island answering
            const float y = top + 13 + k * 6.0f + roll * 3.0f;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.75f - 0.35f * k) * ch));
            dc->DrawLine(D2D1::Point2F(cx - 4, y), D2D1::Point2F(cx, y + 3), brush_.Get(), 1.5f, TourStroke());
            dc->DrawLine(D2D1::Point2F(cx, y + 3), D2D1::Point2F(cx + 4, y), brush_.Get(), 1.5f, TourStroke());
        }
    }
    DrawCursor(dc, p, (reducedMotion_ ? 1.0f : Seg(u, 0.0f, 0.25f) * (1 - Seg(u, 4.8f, 5.3f))) * a, 0);
    TourCaption(dc, s, L"Scroll on the island to open it", reducedMotion_ ? 0 : Seg(u, 0.9f, 1.15f) * (1 - Seg(u, 2.2f, 2.45f)) * a);
    TourCaption(dc, s, L"Move away and it tucks itself back", reducedMotion_ ? 0 : Seg(u, 3.3f, 3.55f) * (1 - Seg(u, 4.55f, 4.8f)) * a);
}

// The closed island changes what it shows: the song's line, an agent at work, a login code.
void IslandWindow::SceneGlance(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time) {
    constexpr float kEach = 2.3f;
    const float u = reducedMotion_ ? 1.2f : std::fmod(time, kEach * 3);
    const int idx = std::min(2, static_cast<int>(u / kEach)), prev = (idx + 2) % 3;
    const float local = u - idx * kEach;
    TourDesktop(dc, s, a, true);
    const float widths[3] = {std::ceil(gfx_.MeasureWidth(L"Dancing in the evening light", gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD))) + 64, 200, 150};
    const float morph = reducedMotion_ ? 1.0f : EaseInOut(Seg(local, 0, 0.42f));
    const float cx = (s.left + s.right) / 2, top = s.top + 12, h = 28, cy = top + h / 2;
    const float w = Lerp(widths[prev], widths[idx], morph);
    const D2D1_RECT_F r = D2D1::RectF(cx - w / 2, top, cx + w / 2, top + h);
    TourMiniIsland(dc, r, a);
    const float ca = (reducedMotion_ ? 1.0f : Seg(local, 0.2f, 0.48f) * (1 - Seg(local, kEach - 0.32f, kEach))) * a;
    IDWriteTextFormat* f = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (idx == 0) {
        DrawText(dc, L"♪", gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), Centered(D2D1::Point2F(r.left + 15, cy), 16, 18), WithAlpha(kBrandLight, ca),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"Dancing in the evening light", f, D2D1::RectF(r.left + 26, top, r.right - 30, top + h), WithAlpha(kText, ca));
        for (int i = 0; i < 3; ++i) {
            const float bh = 4 + 7 * (0.5f + 0.5f * std::sin(time * (7 + i * 2.3f) + i));
            const float x = r.right - 23 + i * 5.0f;
            brush_->SetColor(WithAlpha(kBrandLight, ca));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, cy + 6 - bh, x + 3, cy + 6), 1.5f, 1.5f}, brush_.Get());
        }
    } else if (idx == 1) {
        if (ID2D1Bitmap1* logo = Logo(AgentTool::Claude)) dc->DrawBitmap(logo, Centered(D2D1::Point2F(r.left + 17, cy), 15, 15), ca, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        DrawText(dc, L"Running the tests", f, D2D1::RectF(r.left + 31, top, r.right - 58, top + h), WithAlpha(kText, ca));
        const D2D1_RECT_F track = D2D1::RectF(r.right - 52, cy - 1.5f, r.right - 14, cy + 1.5f);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.14f * ca));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{track, 1.5f, 1.5f}, brush_.Get());
        brush_->SetColor(WithAlpha(kAccent, ca));
        const float done = reducedMotion_ ? 0.6f : EaseInOut(Seg(local, 0.3f, kEach - 0.3f));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(track.left, track.top, Lerp(track.left, track.right, done), track.bottom), 1.5f, 1.5f}, brush_.Get());
    } else {
        DrawText(dc, L"Code", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 14, top, r.left + 48, top + h), WithAlpha(kText, 0.55f * ca));
        DrawText(dc, L"482 913", gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_BOLD), D2D1::RectF(r.left + 46, top, r.right - 30, top + h), WithAlpha(kText, ca));
        DrawText(dc, L"", gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL, true), Centered(D2D1::Point2F(r.right - 17, cy), 16, 16),
                 WithAlpha(kText, (0.45f + 0.35f * std::sin(time * 4.0f)) * ca), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // Underneath: what each one is, the one showing lit, a line gliding to it.
    const wchar_t* labels[3] = {L"Song lyrics", L"Your agents", L"Login codes"};
    IDWriteTextFormat* lf = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float lw[3], total = 0;
    for (int i = 0; i < 3; ++i) total += lw[i] = std::ceil(gfx_.MeasureWidth(labels[i], lf));
    total += 2 * 22.0f;
    float x = cx - total / 2, lx[3];
    const float y = s.top + 60;
    for (int i = 0; i < 3; ++i) {
        lx[i] = x;
        const float lit = i == idx ? Lerp(0.38f, 0.95f, morph) : i == prev ? Lerp(0.95f, 0.38f, morph) : 0.38f;
        DrawText(dc, labels[i], lf, D2D1::RectF(x, y, x + lw[i] + 2, y + 18), WithAlpha(kText, lit * a));
        x += lw[i] + 22;
    }
    const float ux = Lerp(lx[prev], lx[idx], morph), uw = Lerp(lw[prev], lw[idx], morph);
    brush_->SetColor(WithAlpha(kBrandLight, a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(ux, y + 21, ux + uw, y + 23), 1, 1}, brush_.Get());
}

// Messages arrive one after another and settle into the open island, newest on top.
void IslandWindow::SceneNotifications(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time) {
    const float u = reducedMotion_ ? 3.6f : std::fmod(time, 6.2f);
    TourDesktop(dc, s, a, true);
    const float cx = (s.left + s.right) / 2, top = s.top + 10;
    const float open = reducedMotion_ ? 1.0f : EaseOutBack(Seg(u, 0.05f, 0.5f)) * (1 - EaseInOut(Seg(u, 5.55f, 6.0f)));
    const float w = Lerp(58, 250, open), h = std::max(6.0f, Lerp(6, 142, open));
    const D2D1_RECT_F r = D2D1::RectF(cx - w / 2, top, cx + w / 2, top + h);
    TourMiniIsland(dc, r, a);
    const float ca = Seg(open, 0.72f, 1.0f) * a;
    if (ca < 0.003f) return;
    DrawText(dc, L"Notifications", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 14, top + 7, r.right - 70, top + 23),
             WithAlpha(kText, 0.9f * ca));
    DrawText(dc, L"Clear all", gfx_.Text(9.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.right - 70, top + 7, r.right - 14, top + 23),
             WithAlpha(kText, 0.45f * ca), DWRITE_TEXT_ALIGNMENT_TRAILING);
    struct Toast {
        D2D1_COLOR_F tile;
        const wchar_t* mark;
        bool glyph;
        const wchar_t *who, *what;
    };
    const Toast toasts[3] = {{Rgba(0.15f, 0.78f, 0.38f, 1), L"W", false, L"Maya", L"Dinner at 8 tonight?"},
                             {Rgba(0.33f, 0.12f, 0.36f, 1), L"S", false, L"#design", L"The new mockups are up"},
                             {Rgba(0.04f, 0.52f, 1.0f, 1), L"", true, L"Mail", L"Your order has shipped"}};
    const float arrive[3] = {0.75f, 1.75f, 2.75f};
    constexpr float rowH = 37;
    dc->PushAxisAlignedClip(Inflate(r, -1, -1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (int i = 0; i < 3; ++i) {
        if (!reducedMotion_ && u < arrive[i]) continue;
        const float k = reducedMotion_ ? 1.0f : Seg(u, arrive[i], arrive[i] + 0.55f);
        float pos = 0;                                                     // newer ones push it down
        for (int j = i + 1; j < 3; ++j) pos += reducedMotion_ ? 1.0f : EaseInOut(Seg(u, arrive[j], arrive[j] + 0.35f));
        const float off = (1 - EaseOutBack(k)) * 230;
        const float y = top + 28 + pos * rowH;
        const float ra = Seg(k, 0, 0.35f) * ca;
        const D2D1_RECT_F row = D2D1::RectF(r.left + 10 + off, y, r.right - 10 + off, y + rowH - 4);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.065f * ra));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{row, 9, 9}, brush_.Get());
        const D2D1_RECT_F tile = Centered(D2D1::Point2F(row.left + 17, (row.top + row.bottom) / 2), 20, 20);
        brush_->SetColor(WithAlpha(toasts[i].tile, ra));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{tile, 6, 6}, brush_.Get());
        DrawText(dc, toasts[i].mark, toasts[i].glyph ? gfx_.Text(10.0f, DWRITE_FONT_WEIGHT_NORMAL, true) : gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_BOLD), tile,
                 D2D1::ColorF(1, 1, 1, ra), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, toasts[i].who, gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(row.left + 34, row.top + 3, row.right - 36, row.top + 17),
                 WithAlpha(kText, ra));
        DrawText(dc, L"now", gfx_.Text(9.0f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(row.right - 40, row.top + 3, row.right - 9, row.top + 17),
                 WithAlpha(kText, 0.45f * ra), DWRITE_TEXT_ALIGNMENT_TRAILING);
        DrawText(dc, toasts[i].what, gfx_.Text(10.0f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(row.left + 34, row.top + 16, row.right - 9, row.bottom - 2),
                 WithAlpha(kText, 0.62f * ra));
    }
    dc->PopAxisAlignedClip();
}

// Claude Code asks; the hand clicks Approve; the agent carries on and the tests pass.
void IslandWindow::SceneAgents(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time) {
    const float u = reducedMotion_ ? 1.6f : std::fmod(time, 6.6f);
    TourDesktop(dc, s, a, true);
    const float cx = (s.left + s.right) / 2, top = s.top + 10;
    const float open = reducedMotion_ ? 1.0f : EaseOutBack(Seg(u, 0.05f, 0.5f)) * (1 - EaseInOut(Seg(u, 6.0f, 6.45f)));
    const float w = Lerp(58, 256, open), h = std::max(6.0f, Lerp(6, 142, open));
    const D2D1_RECT_F r = D2D1::RectF(cx - w / 2, top, cx + w / 2, top + h);
    TourMiniIsland(dc, r, a);
    const float ca = Seg(open, 0.72f, 1.0f) * a;
    const float gone = reducedMotion_ ? 0.0f : Seg(u, 2.3f, 2.45f);        // the request steps aside, then the result comes in
    const float done = reducedMotion_ ? 0.0f : Seg(u, 2.5f, 2.75f);
    const float press = reducedMotion_ ? 0.0f : Seg(u, 2.0f, 2.1f) * (1 - Seg(u, 2.2f, 2.35f));
    const float full = 256;                                                // (the buttons sit where the open island puts them)
    const D2D1_RECT_F approve = D2D1::RectF(cx + full / 2 - 14 - 72, top + 104, cx + full / 2 - 14, top + 128);
    const D2D1_RECT_F reject = D2D1::RectF(approve.left - 8 - 64, top + 104, approve.left - 8, top + 128);
    if (const float pa = ca * (1 - gone); pa > 0.003f) {
        if (ID2D1Bitmap1* logo = Logo(AgentTool::Claude)) dc->DrawBitmap(logo, Centered(D2D1::Point2F(r.left + 21, top + 19), 15, 15), pa, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        IDWriteTextFormat* strong = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float nw = std::ceil(gfx_.MeasureWidth(L"Claude Code", strong));
        DrawText(dc, L"Claude Code", strong, D2D1::RectF(r.left + 34, top + 11, r.left + 34 + nw + 2, top + 27), WithAlpha(kText, pa));
        DrawText(dc, L" wants to run a command", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 34 + nw, top + 11, r.right - 12, top + 27),
                 WithAlpha(kText, 0.72f * pa));
        DrawText(dc, L"in eflow", gfx_.Text(9.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 34, top + 27, r.right - 12, top + 41), WithAlpha(kText, 0.45f * pa));
        const D2D1_RECT_F box = D2D1::RectF(r.left + 14, top + 50, r.right - 14, top + 82);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * pa));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{box, 7, 7}, brush_.Get());
        DrawText(dc, L"npm test", gfx_.Mono(11.0f), D2D1::RectF(box.left + 10, box.top, box.right - 10, box.bottom), WithAlpha(kText, 0.92f * pa));
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * pa));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{reject, 12, 12}, brush_.Get());
        DrawText(dc, L"Reject", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), reject, WithAlpha(kText, pa), DWRITE_TEXT_ALIGNMENT_CENTER);
        brush_->SetColor(D2D1::ColorF(kButtonBlue.r + 0.22f * press, kButtonBlue.g + 0.14f * press, 1.0f, pa));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{approve, 12, 12}, brush_.Get());
        DrawText(dc, L"Approve", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), approve, D2D1::ColorF(1, 1, 1, pa), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (const float da = ca * done; da > 0.003f) {
        const D2D1_RECT_F badge = Centered(D2D1::Point2F(r.left + 25, top + 25), 22, 22);
        brush_->SetColor(WithAlpha(kGreen, 0.20f * da));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 25, top + 25), 11, 11), brush_.Get());
        DrawText(dc, L"", gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL, true), badge, WithAlpha(kGreen, da), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"Approved", gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 44, top + 15, r.right - 14, top + 35), WithAlpha(kGreen, da));
        DrawText(dc, L"Claude Code carries on", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 14, top + 46, r.right - 14, top + 62),
                 WithAlpha(kText, 0.66f * da));
        const float fr = EaseInOut(Seg(u, 2.7f, 5.4f));
        const D2D1_RECT_F track = D2D1::RectF(r.left + 14, top + 74, r.right - 14, top + 78);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * da));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{track, 2, 2}, brush_.Get());
        brush_->SetColor(WithAlpha(fr >= 1.0f ? kGreen : kAccent, da));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(track.left, track.top, Lerp(track.left, track.right, fr), track.bottom), 2, 2}, brush_.Get());
        DrawText(dc, fr < 1.0f ? L"Running npm test…" : L"All 12 tests passed", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                 D2D1::RectF(r.left + 14, top + 86, r.right - 14, top + 102), fr < 1.0f ? WithAlpha(kText, 0.66f * da) : WithAlpha(kGreen, da));
    }
    if (reducedMotion_) return;
    const D2D1_POINT_2F start{s.right - 34, s.bottom - 30}, target{(approve.left + approve.right) / 2 + 4, (approve.top + approve.bottom) / 2 + 2},
        away{s.right - 70, s.bottom - 44};
    D2D1_POINT_2F p;
    if (u < 2.0f) p = Arc(start, D2D1::Point2F(s.right - 20, top + 96), target, EaseInOut(Seg(u, 0.9f, 1.95f)));
    else if (u < 2.7f) p = target;
    else p = Arc(target, D2D1::Point2F(target.x + 30, target.y + 40), away, EaseInOut(Seg(u, 2.7f, 3.4f)));
    TourRipple(dc, target, Seg(u, 2.05f, 2.6f), a);
    DrawCursor(dc, p, Seg(u, 0.8f, 1.0f) * (1 - Seg(u, 3.3f, 3.7f)) * a, press);
}

// A file dragged onto the island goes out with SafeShare; then the island shows a QR code and a phone picks the file up.
void IslandWindow::SceneFiles(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time) {
    const float u = reducedMotion_ ? 1.9f : std::fmod(time, 7.6f);
    TourDesktop(dc, s, a, true);
    const float cx = (s.left + s.right) / 2, top = s.top + 10;
    const float chooser = EaseOutBack(Seg(u, 1.25f, 1.65f)) * (1 - EaseInOut(Seg(u, 3.3f, 3.7f)));
    const float qr = EaseOutBack(Seg(u, 3.95f, 4.35f)) * (1 - EaseInOut(Seg(u, 7.0f, 7.4f)));
    float w = 58, h = 6;
    if (chooser > 0.001f) w = Lerp(58, 240, chooser), h = std::max(6.0f, Lerp(6, 86, chooser));
    else if (qr > 0.001f) w = Lerp(58, 140, qr), h = std::max(6.0f, Lerp(6, 104, qr));
    const D2D1_RECT_F r = D2D1::RectF(cx - w / 2, top, cx + w / 2, top + h);
    TourMiniIsland(dc, r, a);

    // Part one: the two drop zones, SafeShare lighting up under the file.
    const D2D1_POINT_2F drop{cx + 92, top + 22};                          // (clear of the zone's words)
    if (const float za = Seg(chooser, 0.72f, 1.0f) * a; za > 0.003f) {
        const D2D1_RECT_F zones[2] = {D2D1::RectF(r.left + 10, top + 10, cx - 4, r.bottom - 10), D2D1::RectF(cx + 4, top + 10, r.right - 10, r.bottom - 10)};
        for (int i = 0; i < 2; ++i) {
            const float hl = i == 1 ? (reducedMotion_ ? 1.0f : Seg(u, 1.62f, 1.8f)) : 0.0f;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.05f * za));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{zones[i], 9, 9}, brush_.Get());
            brush_->SetColor(WithAlpha(kBrand, 0.34f * hl * za));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{zones[i], 9, 9}, brush_.Get());
            brush_->SetColor(hl > 0 ? WithAlpha(kBrandLight, (0.25f + 0.55f * hl) * za) : D2D1::ColorF(1, 1, 1, 0.14f * za));
            dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{Inflate(zones[i], -0.5f, -0.5f), 8.5f, 8.5f}, brush_.Get(), 1.0f);
            const float zx = (zones[i].left + zones[i].right) / 2, zy = (zones[i].top + zones[i].bottom) / 2;
            const bool sent = i == 1 && u >= 2.7f && !reducedMotion_;
            const bool sealing = i == 1 && u >= 2.35f && u < 2.7f && !reducedMotion_;
            DrawText(dc, sent ? L"" : i == 0 ? L"" : L"", gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_NORMAL, true), Centered(D2D1::Point2F(zx, zy - 8), 20, 20),
                     sent ? WithAlpha(kGreen, za) : WithAlpha(kText, (0.75f + 0.25f * hl) * za), DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawText(dc, sent ? L"Sent" : sealing ? L"Encrypting…" : i == 0 ? L"Shelf" : L"SafeShare", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                     D2D1::RectF(zones[i].left, zy + 3, zones[i].right, zy + 19), sent ? WithAlpha(kGreen, za) : WithAlpha(kText, (0.78f + 0.22f * hl) * za),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    if (const float fly = Seg(u, 2.65f, 3.35f); fly > 0 && fly < 1 && !reducedMotion_) {       // the email, on its way
        const D2D1_POINT_2F from{cx + 62, top + 40}, to{s.right + 14, top + 16};
        const D2D1_POINT_2F p = Arc(from, D2D1::Point2F(to.x - 24, from.y + 46), to, EaseInOut(fly));
        const float ea = (1 - Seg(fly, 0.75f, 1.0f)) * a;
        const D2D1_RECT_F env = Centered(p, 22, 15);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, ea));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{env, 3, 3}, brush_.Get());
        brush_->SetColor(WithAlpha(kBrandDeep, ea));
        dc->DrawLine(D2D1::Point2F(env.left + 2, env.top + 2), D2D1::Point2F(p.x, p.y + 1.5f), brush_.Get(), 1.3f, TourStroke());
        dc->DrawLine(D2D1::Point2F(p.x, p.y + 1.5f), D2D1::Point2F(env.right - 2, env.top + 2), brush_.Get(), 1.3f, TourStroke());
    }

    // The file and the hand that carries it.
    const D2D1_POINT_2F home{s.left + 50, s.bottom - 70};
    const float dropped = reducedMotion_ ? 0.0f : Seg(u, 2.1f, 2.35f);
    if (u < 3.7f) {
        D2D1_POINT_2F hand;
        if (reducedMotion_) hand = drop;
        else if (u < 1.0f) hand = Arc(D2D1::Point2F(s.left + 150, s.bottom - 30), D2D1::Point2F(s.left + 110, s.bottom - 84), D2D1::Point2F(home.x + 6, home.y + 8),
                                      EaseInOut(Seg(u, 0.15f, 0.85f)));
        else hand = Arc(D2D1::Point2F(home.x + 6, home.y + 8), D2D1::Point2F(home.x + 14, top + 34), drop, EaseInOut(Seg(u, 1.0f, 2.0f)));
        const D2D1_POINT_2F filePos = (u < 1.0f && !reducedMotion_) ? home : D2D1::Point2F(hand.x - 6, hand.y - 8);
        const float scale = 1.15f * (1 - 0.7f * dropped), fa = (1 - dropped) * a;
        if (ID2D1PathGeometry* page = TourPath(3); page && fa > 0.003f) {
            D2D1_MATRIX_3X2_F old;
            dc->GetTransform(&old);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(filePos.x, filePos.y) * old);
            brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.25f * fa));
            dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(filePos.x + 1, filePos.y + 2) * old);
            dc->FillGeometry(page, brush_.Get());
            dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(filePos.x, filePos.y) * old);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * fa));
            dc->FillGeometry(page, brush_.Get());
            brush_->SetColor(WithAlpha(kBrandLight, 0.85f * fa));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(3, 11, 17, 21), 2, 2}, brush_.Get());
            brush_->SetColor(D2D1::ColorF(0.75f, 0.76f, 0.82f, fa));
            dc->DrawLine(D2D1::Point2F(13, 0.5f), D2D1::Point2F(13, 7), brush_.Get(), 1.0f);
            dc->DrawLine(D2D1::Point2F(13, 7), D2D1::Point2F(19.5f, 7), brush_.Get(), 1.0f);
            dc->SetTransform(old);
        }
        if (u < 1.0f || reducedMotion_)
            DrawText(dc, L"photo.jpg", gfx_.Text(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(home.x - 22, home.y + 33, home.x + 45, home.y + 47),
                     WithAlpha(kText, (reducedMotion_ ? 0.0f : 0.85f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!reducedMotion_) {
            TourRipple(dc, drop, Seg(u, 2.08f, 2.5f), a);
            DrawCursor(dc, hand, Seg(u, 0.1f, 0.3f) * (1 - Seg(u, 2.4f, 2.8f)) * a, Seg(u, 0.85f, 1.0f) * (1 - Seg(u, 2.05f, 2.2f)));
        }
    }

    // Part two: the island shows a QR code; the phone scans it and downloads the file over Wi-Fi.
    if (const float qa = Seg(qr, 0.72f, 1.0f) * a; qa > 0.003f) {
        DrawText(dc, L"Scan with your phone", gfx_.Text(9.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left, top + 6, r.right, top + 19),
                 WithAlpha(kText, 0.6f * qa), DWRITE_TEXT_ALIGNMENT_CENTER);
        TourQr(dc, Centered(D2D1::Point2F(cx, top + 59), 68, 68), qa, true);
    }
    const float ph = EaseOutBack(Seg(u, 3.8f, 4.3f)) * (1 - EaseInOut(Seg(u, 7.0f, 7.45f)));
    if (ph > 0.001f) {
        const D2D1_POINT_2F pc{Lerp(s.right + 44, s.right - 50, ph), s.bottom - 84};
        const D2D1_RECT_F body = Centered(pc, 62, 116), screen = Inflate(body, -4, -4);
        brush_->SetColor(D2D1::ColorF(0.10f, 0.10f, 0.13f, a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{body, 12, 12}, brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.28f * a));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{body, 12, 12}, brush_.Get(), 1.4f);
        const bool app = u >= 5.35f;
        brush_->SetColor(app ? D2D1::ColorF(0.95f, 0.95f, 0.97f, a) : D2D1::ColorF(0.05f, 0.05f, 0.07f, a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{screen, 9, 9}, brush_.Get());
        if (!app && u >= 4.35f) {                                          // the camera, finding the code
            const D2D1_RECT_F sq = Centered(D2D1::Point2F(pc.x, pc.y - 4), 34, 34);
            TourQr(dc, Inflate(sq, -3, -3), 0.85f * a, false);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, a));
            for (int c = 0; c < 4; ++c) {
                const float x = c % 2 ? sq.right : sq.left, y = c / 2 ? sq.bottom : sq.top, dx = c % 2 ? -7.0f : 7.0f, dy = c / 2 ? -7.0f : 7.0f;
                dc->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + dx, y), brush_.Get(), 1.6f, TourStroke());
                dc->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x, y + dy), brush_.Get(), 1.6f, TourStroke());
            }
            const float sy = Lerp(sq.top + 2, sq.bottom - 2, 0.5f + 0.5f * std::sin((u - 4.35f) * 5.0f));
            brush_->SetColor(WithAlpha(kBrandLight, 0.9f * a));
            dc->DrawLine(D2D1::Point2F(sq.left + 2, sy), D2D1::Point2F(sq.right - 2, sy), brush_.Get(), 1.4f);
        } else if (app) {                                                  // the phone page: the file, and Download
            DrawLogoMark(dc, D2D1::Point2F(screen.left + 11, screen.top + 12), 11, a, true);
            DrawText(dc, L"Eflow", gfx_.Text(8.5f, DWRITE_FONT_WEIGHT_BOLD), D2D1::RectF(screen.left + 19, screen.top + 5, screen.right - 4, screen.top + 19),
                     D2D1::ColorF(0.09f, 0.09f, 0.10f, a));
            DrawText(dc, L"From your PC", gfx_.Text(7.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(screen.left + 6, screen.top + 26, screen.right - 4, screen.top + 37),
                     D2D1::ColorF(0.42f, 0.44f, 0.47f, a));
            const D2D1_RECT_F card = D2D1::RectF(screen.left + 4, screen.top + 40, screen.right - 4, screen.top + 76);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{card, 6, 6}, brush_.Get());
            DrawText(dc, L"photo.jpg", gfx_.Text(8.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(card.left + 5, card.top + 3, card.right - 3, card.top + 15),
                     D2D1::ColorF(0.09f, 0.09f, 0.10f, a));
            const D2D1_RECT_F btn = D2D1::RectF(card.left + 5, card.top + 18, card.right - 5, card.bottom - 4);
            if (u < 6.05f) {
                const float tap = Seg(u, 5.85f, 5.95f) * (1 - Seg(u, 5.95f, 6.05f));
                brush_->SetColor(D2D1::ColorF(0.04f + 0.2f * tap, 0.49f + 0.15f * tap, 1.0f, a));
                dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{btn, 5, 5}, brush_.Get());
                DrawText(dc, L"Download", gfx_.Text(7.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), btn, D2D1::ColorF(1, 1, 1, a), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                DrawText(dc, L"✓ Saved", gfx_.Text(8.0f, DWRITE_FONT_WEIGHT_BOLD), btn, D2D1::ColorF(0.09f, 0.64f, 0.29f, a), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }
    if (u > 5.4f && u < 6.3f && !reducedMotion_) {                         // the file, travelling over the Wi-Fi
        for (int i = 0; i < 5; ++i) {
            const float f = std::fmod((u - 5.4f) * 1.4f + i * 0.2f, 1.0f);
            const D2D1_POINT_2F p = Arc(D2D1::Point2F(cx + 44, top + 84), D2D1::Point2F(s.right - 70, top + 60), D2D1::Point2F(s.right - 52, s.bottom - 146), f);
            brush_->SetColor(WithAlpha(kBrandLight, std::sin(f * kPi) * (1 - Seg(u, 6.1f, 6.3f)) * a));
            dc->FillEllipse(D2D1::Ellipse(p, 2.2f, 2.2f), brush_.Get());
        }
    }
}

// Solu hears its name and a request, thinks, and answers.
void IslandWindow::SceneSolu(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time) {
    const float u = reducedMotion_ ? 4.8f : std::fmod(time, 7.0f);
    TourDesktop(dc, s, a, false);
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.22f * a));
    dc->FillRectangle(s, brush_.Get());
    const D2D1_POINT_2F o{s.left + 64, (s.top + s.bottom) / 2};
    const float listen = Seg(u, 0.3f, 0.5f) * (1 - Seg(u, 2.2f, 2.4f));
    const float think = Seg(u, 2.3f, 2.5f) * (1 - Seg(u, 3.3f, 3.5f));
    const float speak = Seg(u, 3.4f, 3.6f) * (1 - Seg(u, 5.0f, 5.4f));
    const float voice = std::fabs(std::sin(u * 9.0f) * std::sin(u * 3.7f + 1.0f));
    const float level = listen * (u > 0.6f && u < 2.0f ? voice : 0.15f) + speak * (u > 3.5f && u < 4.8f ? voice : 0.1f);
    const float act = std::max({listen, speak, think * (0.55f + 0.45f * std::sin(u * 6.0f))});
    for (int k = 0; k < 3; ++k) {                                          // rings that breathe with the voice
        const float rr = 30 + k * 9.0f + level * (6 + k * 5.0f);
        brush_->SetColor(WithAlpha(think > 0.5f ? kAmber : kBrandLight, (0.32f - 0.08f * k) * (reducedMotion_ ? 0.5f : act) * a));
        dc->DrawEllipse(D2D1::Ellipse(o, rr, rr), brush_.Get(), 1.4f);
    }
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, kBrandLight}, {1.0f, kBrandDeep}};
    Com<ID2D1GradientStopCollection> coll;
    Com<ID2D1RadialGradientBrush> orb;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &coll)) &&
        SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(o.x - 8, o.y - 10), D2D1::Point2F(0, 0), 34, 34), coll.Get(), &orb))) {
        orb->SetOpacity(a);
        dc->FillEllipse(D2D1::Ellipse(o, 24, 24), orb.Get());
    }
    DrawText(dc, L"", gfx_.Text(16.0f, DWRITE_FONT_WEIGHT_NORMAL, true), Centered(o, 26, 26), D2D1::ColorF(1, 1, 1, a), DWRITE_TEXT_ALIGNMENT_CENTER);

    // The conversation, typed out as it is said.
    const float bx0 = s.left + 122, bx1 = s.right - 14;
    IDWriteTextFormat* line = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* wrap = gfx_.Wrapped(11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    const std::wstring ask = L"Remind me to call Mom at 6", reply = L"Done. I’ll remind you at 6:00 PM.";
    const float fade = reducedMotion_ ? 1.0f : 1 - Seg(u, 6.3f, 6.85f);
    const float askW = std::min(150.0f, std::ceil(gfx_.MeasureWidth(ask.c_str(), line)) + 2);
    const float askH = gfx_.MeasureHeight(ask.c_str(), wrap, askW);
    const D2D1_RECT_F askBox = D2D1::RectF(bx1 - askW - 20, s.top + 34, bx1, s.top + 34 + askH + 14);
    if (const float aa = (reducedMotion_ ? 1.0f : Seg(u, 0.5f, 0.75f)) * fade * a; aa > 0.003f) {
        brush_->SetColor(WithAlpha(kButtonBlue, aa));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{askBox, 12, 12}, brush_.Get());
        const size_t n = reducedMotion_ ? ask.size() : static_cast<size_t>(std::lround(ask.size() * Seg(u, 0.6f, 2.0f)));
        DrawText(dc, ask.substr(0, n).c_str(), wrap, D2D1::RectF(askBox.left + 10, askBox.top + 7, askBox.right - 10, askBox.bottom - 5),
                 D2D1::ColorF(1, 1, 1, aa), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    const float replyW = std::min(bx1 - bx0 - 20, std::ceil(gfx_.MeasureWidth(reply.c_str(), line)) + 2);
    const float replyH = gfx_.MeasureHeight(reply.c_str(), wrap, replyW);
    const D2D1_RECT_F replyBox = D2D1::RectF(bx0, askBox.bottom + 12, bx0 + replyW + 20, askBox.bottom + 12 + replyH + 14);
    if (const float ta = (reducedMotion_ ? 0.0f : think) * fade * a; ta > 0.003f) {
        const D2D1_RECT_F dots = D2D1::RectF(bx0, replyBox.top, bx0 + 46, replyBox.top + 24);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.13f * ta));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{dots, 12, 12}, brush_.Get());
        for (int i = 0; i < 3; ++i) {
            const float ph = 0.5f + 0.5f * std::sin(u * 7.0f - i * 0.9f);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.35f + 0.6f * ph) * ta));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dots.left + 14 + i * 9.0f, dots.top + 12 - ph * 2), 2.4f, 2.4f), brush_.Get());
        }
    }
    if (const float ra = (reducedMotion_ ? 1.0f : Seg(u, 3.35f, 3.6f)) * fade * a; ra > 0.003f) {
        DrawText(dc, L"Solu", gfx_.Text(9.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(replyBox.left + 4, replyBox.bottom + 3, replyBox.right, replyBox.bottom + 16),
                 WithAlpha(kBrandLight, ra));
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.13f * ra));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{replyBox, 12, 12}, brush_.Get());
        const size_t n = reducedMotion_ ? reply.size() : static_cast<size_t>(std::lround(reply.size() * Seg(u, 3.5f, 4.6f)));
        DrawText(dc, reply.substr(0, n).c_str(), wrap, D2D1::RectF(replyBox.left + 10, replyBox.top + 7, replyBox.right - 10, replyBox.bottom - 5),
                 WithAlpha(kText, ra), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    TourCaption(dc, s, L"Say “Solu”, then ask", reducedMotion_ ? 0.0f : Seg(u, 0.2f, 0.45f) * (1 - Seg(u, 2.2f, 2.45f)) * a);
}

// The shortcuts, each pressed in turn.
void IslandWindow::SceneDone(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time) {
    const float u = reducedMotion_ ? 0.6f : std::fmod(time, 5.6f);
    TourDesktop(dc, s, a, false);
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.30f * a));
    dc->FillRectangle(s, brush_.Get());
    struct Row {
        const wchar_t *key, *label;
    };
    const Row rows[4] = {{L"Space", L"Talk to Solu"}, {L"A", L"Write to your agents"}, {L"H", L"Hide everything"}, {L"Q", L"Ask about your screen"}};
    const int active = std::min(3, static_cast<int>(u / 1.4f));
    const float local = u - active * 1.4f;
    IDWriteTextFormat* kf = gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto keyW = [&](const wchar_t* key) { return std::max(30.0f, std::ceil(gfx_.MeasureWidth(key, kf)) + 18); };
    float labelX = 0;                                                      // the labels line up after the widest set of keys
    for (const Row& row : rows) labelX = std::max(labelX, keyW(L"Ctrl") + keyW(L"Alt") + keyW(row.key) + 10);
    for (int i = 0; i < 4; ++i) {
        const float y = s.top + 22 + i * 46.0f;
        const float hl = i == active ? (reducedMotion_ ? 1.0f : Seg(local, 0, 0.2f) * (1 - Seg(local, 1.2f, 1.4f))) : 0.0f;
        float x = s.left + 22;
        const wchar_t* keys[3] = {L"Ctrl", L"Alt", rows[i].key};
        for (int k = 0; k < 3; ++k) {
            const float kw = keyW(keys[k]);
            const float press = i == active && !reducedMotion_ ? Seg(local, 0.12f + 0.12f * k, 0.22f + 0.12f * k) * (1 - Seg(local, 0.95f, 1.1f)) : 0.0f;
            TourKeycap(dc, D2D1::RectF(x, y, x + kw, y + 26), keys[k], press, (0.55f + 0.45f * hl) * a);
            x += kw + 5;
        }
        DrawText(dc, rows[i].label, gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(s.left + 22 + labelX + 6, y, s.right - 14, y + 26), WithAlpha(kText, (0.42f + 0.55f * hl) * a));
    }
}

}  // namespace eflow

