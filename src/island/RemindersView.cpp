// The Reminders tab: upcoming reminders, a box to add one in plain words, and the card shown while one rings.
#include "island/IslandWindow.h"

#include <algorithm>
#include <cmath>

#include "island/IslandStyle.h"

namespace eflow {
namespace {

constexpr float kRowH = 38.0f;
constexpr float kWhatH = 34.0f, kNoteH = 30.0f;

const wchar_t* Glyph(const Reminder& r) {
    if (r.missed) return L"";                               // a warning sign
    return r.repeat == Repeat::None ? L"" : L"";      // a bell, or the repeat arrows
}

std::wstring SecondLine(const Reminder& r) {
    if (!r.note.empty()) return r.note;
    std::wstring s;
    if (r.repeat == Repeat::Daily) s = L"Every day";
    else if (r.repeat == Repeat::Weekdays) s = L"Monday to Friday";
    else if (r.repeat == Repeat::Weekly) s = L"Every week";
    if (r.earlyMinutes > 0) s += (s.empty() ? L"" : L" · ") + std::wstring(L"heads-up ") + FormatEarly(r.earlyMinutes);
    return s;
}

bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom; }

}  // namespace

// ---- layout --------------------------------------------------------------------------------------------------------------

IslandWindow::RemindersLayout IslandWindow::RemindersGeometry(const D2D1_RECT_F& island) const {
    RemindersLayout g{};
    const float x0 = island.left + 22, x1 = island.left + kExpandedW - kMascotLane + 6;
    const float bottom = island.top + kRemindersH - 14;
    g.note = D2D1::RectF(x0, bottom - kNoteH, x1 - 40, bottom);
    g.what = D2D1::RectF(x0, g.note.top - 8 - kWhatH, x1 - 40, g.note.top - 8);
    g.add = D2D1::RectF(x1 - 32, g.what.top + 1, x1, g.what.bottom - 1);
    g.list = D2D1::RectF(x0, island.top + kTabTop + kTabH + 12, x1, g.what.top - 10);
    g.rows = std::max(1, static_cast<int>((g.list.bottom - g.list.top) / kRowH));
    const When w = ReminderPreview();
    if (!reminderWhat_.text.empty()) {                            // the chip inside the box: what was understood
        IDWriteTextFormat* chip = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring label = w.ok ? FormatWhen(w.at, w.repeat, UnixNow()) + (w.meridiemGuessed ? L"  ⇄" : L"") : L"Add a day or time";
        const float width = std::ceil(gfx_.MeasureWidth(label.c_str(), chip)) + 20;
        g.preview = D2D1::RectF(g.what.right - 5 - width, g.what.top + 5, g.what.right - 5, g.what.bottom - 5);
    }
    return g;
}

When IslandWindow::ReminderPreview() const {
    When w = ParseWhen(reminderWhat_.text, UnixNow());
    if (w.ok && w.meridiemGuessed && reminderFlip_) w.at = FlipMeridiem(w.at);   // the user flipped the guess
    return w;
}

std::array<D2D1_RECT_F, 2> IslandWindow::AlarmButtons(const D2D1_RECT_F& island) const {
    const float x1 = island.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    const float bottom = island.top + kAlarmH - 18, top = bottom - 32;
    const D2D1_RECT_F dismiss = D2D1::RectF(x1 - 104, top, x1, bottom);
    const D2D1_RECT_F snooze = D2D1::RectF(dismiss.left - 10 - 120, top, dismiss.left - 10, bottom);
    return {snooze, dismiss};
}

// ---- drawing -------------------------------------------------------------------------------------------------------------

void IslandWindow::DrawReminders(ID2D1DeviceContext* dc, D2D1_RECT_F base, float a) {
    if (!reminders_.ringing.empty()) {
        DrawAlarm(dc, base, a);
        return;
    }
    const RemindersLayout g = RemindersGeometry(base);
    const int64_t now = UnixNow();
    IDWriteTextFormat* title = gfx_.Text(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* caption = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* when = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* icons = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_NORMAL, true);

    // ---- the list
    if (reminders_.items.empty()) {
        const float mid = (g.list.top + g.list.bottom) / 2;
        DrawText(dc, L"No reminders yet", gfx_.Text(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(g.list.left, mid - 24, g.list.right, mid),
                 WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"Type one below, like “Codeforces contest 4 March 8pm”", gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL),
                 D2D1::RectF(g.list.left, mid + 2, g.list.right, mid + 22), WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        reminderScroll_ = std::clamp(reminderScroll_, 0, std::max(0, static_cast<int>(reminders_.items.size()) - g.rows));
        dc->PushAxisAlignedClip(g.list, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int row = 0; row < g.rows; ++row) {
            const size_t i = static_cast<size_t>(reminderScroll_ + row);
            if (i >= reminders_.items.size()) break;
            const Reminder& r = reminders_.items[i];
            const float top = g.list.top + row * kRowH;
            const D2D1_COLOR_F tint = r.missed ? kRed : r.repeat == Repeat::None ? kAccent : kGreen;
            const float cy = top + kRowH / 2;
            brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, 0.16f * a));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(g.list.left + 14, cy), 14.0f, 14.0f), brush_.Get());
            DrawText(dc, Glyph(r), icons, D2D1::RectF(g.list.left, cy - 14, g.list.left + 28, cy + 14), WithAlpha(tint, a), DWRITE_TEXT_ALIGNMENT_CENTER);

            const float textLeft = g.list.left + 38, timeRight = g.list.right - 30;
            const std::wstring whenText = r.missed ? L"Missed · " + FormatWhen(r.at, Repeat::None, now) : FormatWhen(r.at, r.repeat, now);
            const float whenW = std::ceil(gfx_.MeasureWidth(whenText.c_str(), when));
            const std::wstring second = SecondLine(r);
            const float titleBottom = second.empty() ? top + kRowH : top + 21;
            DrawText(dc, r.title.c_str(), title, D2D1::RectF(textLeft, second.empty() ? top : top + 3, timeRight - whenW - 12, titleBottom),
                     WithAlpha(kText, (r.missed ? 0.6f : 1.0f) * a));
            if (!second.empty())
                DrawText(dc, second.c_str(), caption, D2D1::RectF(textLeft, top + 20, timeRight - whenW - 12, top + 36), WithAlpha(kTextDim, a));
            DrawText(dc, whenText.c_str(), when, D2D1::RectF(timeRight - whenW, top + 3, timeRight, top + 21),
                     WithAlpha(r.missed ? kRed : kText, 0.85f * a), DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (!r.missed)
                DrawText(dc, FormatCountdown(r.at, now).c_str(), caption, D2D1::RectF(timeRight - 120, top + 20, timeRight, top + 36),
                         WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_TRAILING);
            const bool hot = hover_ == kHitReminderRemove + row;   // delete
            DrawText(dc, L"", gfx_.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(g.list.right - 24, cy - 12, g.list.right, cy + 12),
                     WithAlpha(hot ? kRed : kTextDim, (hot ? 1.0f : 0.7f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        dc->PopAxisAlignedClip();
        const int hidden = static_cast<int>(reminders_.items.size()) - reminderScroll_ - g.rows;
        if (hidden > 0)
            DrawText(dc, (L"+" + std::to_wstring(hidden) + L" more").c_str(), caption, D2D1::RectF(g.list.right - 90, g.list.bottom - 2, g.list.right, g.list.bottom + 10),
                     WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // ---- the boxes: what and when, then an optional note
    const When w = ReminderPreview();
    const bool typed = !reminderWhat_.text.empty();
    const bool past = w.ok && w.repeat == Repeat::None && w.at <= UnixNow();
    DrawField(dc, reminderWhat_, g.what, L"New reminder: “Codeforces contest 4 March 8pm”", a,
              typed ? (g.preview.right - g.preview.left) + 8 : 0);
    if (typed) {
        const D2D1_COLOR_F c = !w.ok ? kTextDim : past ? kRed : kAccent;
        brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, (hover_ == kHitReminderMeridiem ? 0.26f : 0.16f) * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{g.preview, 12, 12}, brush_.Get());
        std::wstring label = !w.ok ? L"Add a day or time" : past ? L"That time has passed" : FormatWhen(w.at, w.repeat, UnixNow());
        if (w.ok && !past && w.meridiemGuessed) label += L"  ⇄";             // click to flip AM / PM
        DrawText(dc, label.c_str(), gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), g.preview, WithAlpha(c, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    std::wstring noteHint = L"Add a note (optional)";
    if (typed && w.ok && w.earlyMinutes > 0) noteHint = L"Heads-up " + FormatEarly(w.earlyMinutes) + L" · add a note (optional)";
    DrawField(dc, reminderNote_, g.note, noteHint.c_str(), a);
    const bool canAdd = typed && w.ok && !past;
    brush_->SetColor(D2D1::ColorF(0.04f + (hover_ == kHitReminderAdd && canAdd ? 0.06f : 0.0f), 0.47f, 1.0f, (canAdd ? 1.0f : 0.32f) * a));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((g.add.left + g.add.right) / 2, (g.add.top + g.add.bottom) / 2), 16.0f, 16.0f), brush_.Get());
    DrawText(dc, L"", gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL, true), g.add, WithAlpha(D2D1::ColorF(1, 1, 1, 1), (canAdd ? 1.0f : 0.6f) * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void IslandWindow::DrawAlarm(ID2D1DeviceContext* dc, D2D1_RECT_F base, float a) {
    const Alert& alert = reminders_.ringing.front();
    const float x0 = base.left + 24, x1 = base.left + kExpandedW - (mascot_ ? kMascotLane : 24.0f);
    const float top = base.top + kTabTop + kTabH + 16;
    const D2D1_COLOR_F tint = alert.early ? kAmber : kAccent;

    brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, 0.18f * a));          // the bell
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + 22, top + 30), 22.0f, 22.0f), brush_.Get());
    DrawText(dc, L"", gfx_.Text(19.0f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(x0, top + 8, x0 + 44, top + 52), WithAlpha(tint, a),
             DWRITE_TEXT_ALIGNMENT_CENTER);

    const float tx = x0 + 58;
    std::wstring kicker = alert.early ? L"Coming up " + FormatCountdown(alert.at, UnixNow()) : L"Reminder";
    if (reminders_.ringing.size() > 1) kicker += L"  ·  1 of " + std::to_wstring(reminders_.ringing.size());
    DrawText(dc, kicker.c_str(), gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(tx, top + 2, x1, top + 18), WithAlpha(tint, a));
    DrawText(dc, alert.title.c_str(), gfx_.Text(20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(tx, top + 18, x1, top + 46), WithAlpha(kText, a));
    const std::wstring time = FormatWhen(alert.at, Repeat::None, UnixNow());
    const std::wstring detail = alert.note.empty() ? time : time + L"  ·  " + alert.note;
    DrawText(dc, detail.c_str(), gfx_.Wrapped(12.5f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(tx, top + 48, x1, top + 84), WithAlpha(kTextDim, 1.15f * a),
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    const auto buttons = AlarmButtons(base);
    IDWriteTextFormat* font = gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (hover_ == kHitAlarmSnooze ? 0.16f : 0.10f) * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{buttons[0], 16, 16}, brush_.Get());
    DrawText(dc, alert.early ? L"Remind me then" : L"Snooze 5 min", font, buttons[0], WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    const float hd = hover_ == kHitAlarmDismiss ? 0.06f : 0.0f;
    brush_->SetColor(D2D1::ColorF(0.04f + hd, 0.47f + hd, 1.0f, a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{buttons[1], 16, 16}, brush_.Get());
    DrawText(dc, alert.early ? L"Got it" : L"Dismiss", font, buttons[1], D2D1::ColorF(1, 1, 1, a), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---- input ---------------------------------------------------------------------------------------------------------------

int IslandWindow::RemindersHit(D2D1_POINT_2F p, const D2D1_RECT_F& island) const {
    if (!reminders_.ringing.empty()) {
        const auto buttons = AlarmButtons(island);
        if (In(buttons[0], p)) return kHitAlarmSnooze;
        if (In(buttons[1], p)) return kHitAlarmDismiss;
        return -1;
    }
    const RemindersLayout g = RemindersGeometry(island);
    if (!reminderWhat_.text.empty() && In(g.preview, p)) return kHitReminderMeridiem;
    if (In(g.add, p)) return kHitReminderAdd;
    if (In(g.what, p)) return kHitReminderWhat;
    if (In(g.note, p)) return kHitReminderNote;
    if (In(g.list, p) && p.x >= g.list.right - 26) {
        const int row = static_cast<int>((p.y - g.list.top) / kRowH);
        if (reminderScroll_ + row < static_cast<int>(reminders_.items.size())) return kHitReminderRemove + row;
    }
    return -1;
}

void IslandWindow::RemindersClick(int id) {
    if (id == kHitAlarmSnooze || id == kHitAlarmDismiss) {
        if (reminders_.ringing.empty()) return;
        const Alert& alert = reminders_.ringing.front();
        if (id == kHitAlarmDismiss || alert.early) {               // "Got it" on a heads-up: the reminder itself still rings
            if (reminderDismiss_) reminderDismiss_(alert.id);
        } else if (reminderSnooze_) {
            reminderSnooze_(alert.id, 5);
        }
    } else if (id == kHitReminderWhat) {
        Focus(reminderWhat_);
    } else if (id == kHitReminderNote) {
        Focus(reminderNote_);
    } else if (id == kHitReminderAdd) {
        AddReminder();
    } else if (id == kHitReminderMeridiem) {
        if (ReminderPreview().meridiemGuessed) reminderFlip_ = !reminderFlip_;
    } else if (id >= kHitReminderRemove && id < kHitReminderRemove + 100) {
        const size_t i = static_cast<size_t>(reminderScroll_ + id - kHitReminderRemove);
        if (i < reminders_.items.size() && reminderRemove_) reminderRemove_(reminders_.items[i].id);
    }
}

bool IslandWindow::RemindersKey(WPARAM vk) {
    if (field_ != &reminderWhat_ && field_ != &reminderNote_) return false;
    if (vk == VK_RETURN) {
        AddReminder();
    } else if (vk == VK_TAB || vk == VK_UP || vk == VK_DOWN) {       // between the two boxes
        Focus(field_ == &reminderWhat_ ? reminderNote_ : reminderWhat_);
    } else {
        return false;
    }
    return true;
}

void IslandWindow::AddReminder() {
    const When w = ReminderPreview();
    if (!w.ok || (w.repeat == Repeat::None && w.at <= UnixNow()) || !reminderAdd_) return;
    std::wstring note = reminderNote_.text;
    while (!note.empty() && note.back() == L' ') note.pop_back();
    reminderAdd_(w.title, note, w.at, w.repeat, w.earlyMinutes);
    reminderWhat_.Clear();
    reminderNote_.Clear();
    reminderFlip_ = false;
    reminderScroll_ = 0;
    if (field_) Focus(reminderWhat_);                                // ready for the next one
}

} // namespace eflow
