// The SafeShare tab: drop files, then send them by email through SafeShare (an encrypted room that lasts 10 hours) or to a
// phone on the same Wi-Fi. The sending itself is done by the app's services; this file is the view: layout, drawing, input.
#include "island/IslandWindow.h"

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "core/Clipboard.h"
#include "core/When.h"
#include "island/IslandStyle.h"
#include "services/ShareCrypto.h"

namespace eflow {
namespace {

constexpr float kHeaderH = 26.0f;
constexpr float kChipH = 28.0f, kChipMaxText = 150.0f;
constexpr float kFieldH = 34.0f;
constexpr float kRowH = 46.0f;
constexpr float kQrSide = 100.0f;
constexpr int kDeviceChoices[3] = {1, 3, 5};
constexpr ULONGLONG kNoteMs = 4000, kConfirmMs = 3000;

bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom; }

std::wstring Trimmed(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L' '), b = s.find_last_not_of(L' ');
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

std::wstring FormatBytes(uint64_t bytes) {
    wchar_t buf[32];
    if (bytes < 1024) swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024) swprintf_s(buf, L"%.0f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024));
    else swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

std::wstring FormatLeft(int64_t expiresAt, int64_t now) {
    const int64_t left = expiresAt - now;
    if (left <= 0) return L"Closed";
    if (left < 3600) return std::to_wstring(std::max<int64_t>(1, left / 60)) + L" min left";
    return std::to_wstring((left + 1799) / 3600) + L" h left";
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

}  // namespace

// ---- layout --------------------------------------------------------------------------------------------------------------

IslandWindow::ShareLayout IslandWindow::ShareGeometry(const D2D1_RECT_F& island) const {
    ShareLayout g{};
    const float x0 = island.left + 22, x1 = island.left + kExpandedW - kMascotLane + 6;
    const float top = island.top + kTabTop + kTabH + 10;
    g.modeEmail = D2D1::RectF(x0, top, x0 + 124, top + kHeaderH);
    g.modePhone = D2D1::RectF(x0 + 124, top, x0 + 248, top + kHeaderH);
    g.back = D2D1::RectF(x0, top, x0 + 76, top + kHeaderH);
    IDWriteTextFormat* link = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::wstring sent = L"Sent · " + std::to_wstring(shareSnap_.sent.size());
    const float sentW = std::ceil(gfx_.MeasureWidth(sent.c_str(), link)) + 40;
    g.sentList = D2D1::RectF(x1 - sentW, top, x1, top + kHeaderH);

    g.files = D2D1::RectF(x0, top + kHeaderH + 8, x1, top + kHeaderH + 80);
    g.clear = D2D1::RectF(g.files.right - 58, g.files.bottom - 28, g.files.right - 8, g.files.bottom - 6);
    g.addMore = D2D1::RectF(g.clear.left - 86, g.clear.top, g.clear.left - 6, g.clear.bottom);

    const float form = g.files.bottom + 10;
    g.name = D2D1::RectF(x0, form, x0 + 186, form + kFieldH);
    g.email = D2D1::RectF(x0 + 194, form, x1, form + kFieldH);
    const float row2 = form + kFieldH + 8;
    g.password = D2D1::RectF(x0, row2, x0 + 246, row2 + kFieldH);
    g.generate = D2D1::RectF(g.password.right - 32, row2 + 3, g.password.right - 4, row2 + kFieldH - 3);
    g.reveal = D2D1::RectF(g.generate.left - 30, row2 + 3, g.generate.left - 2, row2 + kFieldH - 3);
    for (int i = 0; i < 3; ++i) {
        const float left = g.password.right + 10 + i * 70.0f;
        g.devices[static_cast<size_t>(i)] = D2D1::RectF(left, row2 + 3, left + 70, row2 + kFieldH - 3);
    }
    g.send = D2D1::RectF(x1 - 100, row2, x1, row2 + kFieldH);
    g.status = D2D1::RectF(x0, row2 + kFieldH + 8, x1, row2 + kFieldH + 30);

    g.panel = D2D1::RectF(x0, form, x1, sharePhone_ ? g.status.bottom : row2 + kFieldH);
    g.primary = D2D1::RectF(x1 - 104, g.panel.bottom - 32, x1, g.panel.bottom);
    g.secondary = D2D1::RectF(g.primary.left - 128, g.primary.top, g.primary.left - 8, g.primary.bottom);
    g.tertiary = D2D1::RectF(g.secondary.left - 136, g.primary.top, g.secondary.left - 8, g.primary.bottom);

    g.list = D2D1::RectF(x0, g.files.top, x1, g.status.bottom);
    g.rows = std::max(1, static_cast<int>((g.list.bottom - g.list.top) / kRowH));
    return g;
}

std::vector<IslandWindow::ShareChip> IslandWindow::ShareChips(const ShareLayout& g) const {
    std::vector<ShareChip> chips;
    IDWriteTextFormat* name = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* size = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    const float top = g.files.top + 10, right = g.files.right - 10;
    constexpr float kMoreW = 74.0f;                                   // room kept for "+12 more"
    float x = g.files.left + 10;
    for (size_t i = 0; i < shareFiles_.size(); ++i) {
        const ShareFile& f = shareFiles_[i];
        const float textW = std::min(kChipMaxText, std::ceil(gfx_.MeasureWidth(f.name.c_str(), name)));
        const float w = 30 + textW + 6 + std::ceil(gfx_.MeasureWidth(FormatBytes(f.size).c_str(), size)) + 30;
        const bool last = i + 1 == shareFiles_.size();
        if (x + w > (last ? right : right - kMoreW - 8)) {
            chips.push_back({D2D1::RectF(x, top, std::min(right, x + kMoreW), top + kChipH), {}, -1});
            break;
        }
        const D2D1_RECT_F box = D2D1::RectF(x, top, x + w, top + kChipH);
        chips.push_back({box, D2D1::RectF(box.right - 26, top + 2, box.right - 4, top + kChipH - 2), static_cast<int>(i)});
        x += w + 8;
    }
    return chips;
}

// ---- drawing -------------------------------------------------------------------------------------------------------------

void IslandWindow::DrawSafeShare(ID2D1DeviceContext* dc, D2D1_RECT_F base, float a) {
    const ShareLayout g = ShareGeometry(base);
    IDWriteTextFormat* tabFont = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    if (shareSentView_) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (hover_ == kHitShareBack ? 0.12f : 0.07f) * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{g.back, kHeaderH / 2, kHeaderH / 2}, brush_.Get());
        DrawText(dc, L"‹  Back", tabFont, g.back, WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"Rooms you sent · each closes 10 hours after sending", gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
                 D2D1::RectF(g.back.right + 14, g.back.top, g.sentList.right, g.back.bottom), WithAlpha(kTextDim, a));
        DrawShareSent(dc, g, a);
        return;
    }

    // ---- the header: Email | Phone, and the sent rooms
    const D2D1_RECT_F seg = D2D1::RectF(g.modeEmail.left, g.modeEmail.top, g.modePhone.right, g.modePhone.bottom);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{seg, kHeaderH / 2, kHeaderH / 2}, brush_.Get());
    const D2D1_RECT_F on = sharePhone_ ? g.modePhone : g.modeEmail;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.15f * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(on.left + 2, on.top + 2, on.right - 2, on.bottom - 2), kHeaderH / 2 - 2, kHeaderH / 2 - 2},
                             brush_.Get());
    DrawText(dc, L"Send by email", tabFont, g.modeEmail, WithAlpha(kText, (sharePhone_ ? (hover_ == kHitShareModeEmail ? 0.85f : 0.6f) : 1.0f) * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(dc, L"Phone ↔ PC", tabFont, g.modePhone, WithAlpha(kText, (!sharePhone_ ? (hover_ == kHitShareModePhone ? 0.85f : 0.6f) : 1.0f) * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!shareSnap_.sent.empty()) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (hover_ == kHitShareSentList ? 0.12f : 0.06f) * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{g.sentList, kHeaderH / 2, kHeaderH / 2}, brush_.Get());
        const std::wstring sent = L"Sent · " + std::to_wstring(shareSnap_.sent.size());
        DrawText(dc, sent.c_str(), tabFont, D2D1::RectF(g.sentList.left, g.sentList.top, g.sentList.right - 14, g.sentList.bottom),
                 WithAlpha(kText, 0.85f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"›", tabFont, D2D1::RectF(g.sentList.right - 22, g.sentList.top - 1, g.sentList.right - 8, g.sentList.bottom - 1),
                 WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    DrawShareFiles(dc, g, a);
    if (sharePhone_) {
        DrawSharePhone(dc, g, a);
        return;
    }
    const SharePhase phase = shareSnap_.transfer.phase;
    if (ShareBusy()) DrawShareProgress(dc, g, a);
    else if (phase == SharePhase::Sent) DrawShareResult(dc, g, a);
    else DrawShareForm(dc, g, a);
    DrawShareStatus(dc, g, a);
}

void IslandWindow::DrawShareFiles(ID2D1DeviceContext* dc, const ShareLayout& g, float a) {
    const bool hot = hover_ == kHitShareAddFiles;
    const float dim = ShareBusy() ? 0.55f : 1.0f;                     // the files being sent: shown, not editable
    const D2D1_ROUNDED_RECT area{g.files, 12, 12};
    brush_->SetColor(D2D1::ColorF(1, 1, 1, (hot && shareFiles_.empty() ? 0.07f : 0.045f) * a));   // (a drag shows the drop zones instead)
    dc->FillRoundedRectangle(area, brush_.Get());

    if (shareFiles_.empty()) {
        Com<ID2D1StrokeStyle> dashed;
        const D2D1_STROKE_STYLE_PROPERTIES dash = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                              D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
        const float dashes[] = {4.0f, 4.0f};
        gfx_.d2dFactory()->CreateStrokeStyle(dash, dashes, 2, &dashed);
        brush_->SetColor(D2D1::ColorF(kTextDim.r, kTextDim.g, kTextDim.b, 0.45f * a));
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(g.files.left + 0.5f, g.files.top + 0.5f, g.files.right - 0.5f, g.files.bottom - 0.5f), 12, 12},
                                 brush_.Get(), 1.2f, dashed.Get());
        const float cy = (g.files.top + g.files.bottom) / 2;
        DrawText(dc, L"", gfx_.Text(18.0f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(g.files.left, cy - 30, g.files.right, cy - 6),
                 WithAlpha(kText, 0.85f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"Drop files here", gfx_.Text(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                 D2D1::RectF(g.files.left, cy - 6, g.files.right, cy + 12), WithAlpha(kText, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(dc, L"or click to choose · up to 100 files, 4 GB", gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
                 D2D1::RectF(g.files.left, cy + 12, g.files.right, cy + 28), WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    IDWriteTextFormat* name = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* sizeFont = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* icons = gfx_.Text(11.0f, DWRITE_FONT_WEIGHT_NORMAL, true);
    const auto chips = ShareChips(g);
    for (size_t c = 0; c < chips.size(); ++c) {
        const ShareChip& chip = chips[c];
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{chip.box, kChipH / 2, kChipH / 2}, brush_.Get());
        if (chip.file < 0) {
            const int more = static_cast<int>(shareFiles_.size()) - static_cast<int>(c);
            DrawText(dc, (L"+" + std::to_wstring(more) + L" more").c_str(), name, chip.box, WithAlpha(kText, 0.8f * dim * a), DWRITE_TEXT_ALIGNMENT_CENTER);
            continue;
        }
        const ShareFile& f = shareFiles_[static_cast<size_t>(chip.file)];
        DrawText(dc, L"", icons, D2D1::RectF(chip.box.left + 8, chip.box.top, chip.box.left + 26, chip.box.bottom), WithAlpha(kAccent, dim * a),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        const float textW = std::min(kChipMaxText, std::ceil(gfx_.MeasureWidth(f.name.c_str(), name)));
        DrawText(dc, f.name.c_str(), name, D2D1::RectF(chip.box.left + 30, chip.box.top, chip.box.left + 30 + textW, chip.box.bottom),
                 WithAlpha(kText, dim * a));
        DrawText(dc, FormatBytes(f.size).c_str(), sizeFont, D2D1::RectF(chip.box.left + 36 + textW, chip.box.top, chip.close.left, chip.box.bottom),
                 WithAlpha(kTextDim, dim * a));
        if (!ShareBusy()) {
            const bool closeHot = hover_ == kHitShareFileRemove + static_cast<int>(c);
            DrawText(dc, L"", gfx_.Text(9.0f, DWRITE_FONT_WEIGHT_NORMAL, true), chip.close, WithAlpha(closeHot ? kRed : kTextDim, (closeHot ? 1.0f : 0.8f) * a),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    uint64_t total = 0;
    for (const ShareFile& f : shareFiles_) total += f.size;
    const std::wstring summary = std::to_wstring(shareFiles_.size()) + (shareFiles_.size() == 1 ? L" file · " : L" files · ") + FormatBytes(total);
    DrawText(dc, summary.c_str(), sizeFont, D2D1::RectF(g.files.left + 14, g.clear.top, g.addMore.left - 8, g.clear.bottom), WithAlpha(kTextDim, a));
    if (!ShareBusy()) {
        DrawText(dc, L"+ Add more", name, g.addMore, WithAlpha(hot ? kAccent : kText, (hot ? 1.0f : 0.75f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        const bool clearHot = hover_ == kHitShareClearFiles;
        DrawText(dc, L"Clear", name, g.clear, WithAlpha(clearHot ? kRed : kText, (clearHot ? 1.0f : 0.75f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void IslandWindow::DrawShareButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, const wchar_t* label, bool primary, bool enabled, bool hot, float a) {
    const float radius = (r.bottom - r.top) / 2;
    const float lift = hot && enabled ? 0.06f : 0.0f;
    if (primary) brush_->SetColor(D2D1::ColorF(0.04f + lift, 0.47f + lift, 1.0f, (enabled ? 1.0f : 0.35f) * a));
    else brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.10f + lift) * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{r, radius, radius}, brush_.Get());
    DrawText(dc, label, gfx_.Text(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), r,
             primary ? D2D1::ColorF(1, 1, 1, (enabled ? 1.0f : 0.6f) * a) : WithAlpha(kText, (enabled ? 1.0f : 0.5f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
}

void IslandWindow::DrawShareForm(ID2D1DeviceContext* dc, const ShareLayout& g, float a) {
    DrawField(dc, shareName_, g.name, L"Your name", a);
    DrawField(dc, shareEmail_, g.email, L"Recipient's email", a);
    DrawField(dc, sharePassword_, g.password, L"Password · 8+ characters", a, g.password.right - g.reveal.left - 8, !shareReveal_);
    IDWriteTextFormat* icons = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL, true);
    const bool revealHot = hover_ == kHitShareReveal, generateHot = hover_ == kHitShareGenerate;
    DrawText(dc, shareReveal_ ? L"" : L"", icons, g.reveal, WithAlpha(revealHot ? kText : kTextDim, (revealHot ? 1.0f : 0.85f) * a),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(dc, L"", icons, g.generate, WithAlpha(generateHot ? kAccent : kTextDim, (generateHot ? 1.0f : 0.85f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);

    IDWriteTextFormat* chip = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const D2D1_RECT_F group = D2D1::RectF(g.devices[0].left, g.devices[0].top, g.devices[2].right, g.devices[2].bottom);
    const float radius = (group.bottom - group.top) / 2;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{group, radius, radius}, brush_.Get());
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F& r = g.devices[static_cast<size_t>(i)];
        const bool chosen = shareDevices_ == kDeviceChoices[i];
        if (chosen) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.16f * a));
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2), radius - 2, radius - 2}, brush_.Get());
        }
        const wchar_t* label = i == 0 ? L"1 device" : i == 1 ? L"3 devices" : L"5 devices";
        const bool hot = hover_ == kHitShareDevices + i;
        DrawText(dc, label, chip, r, WithAlpha(kText, (chosen ? 1.0f : hot ? 0.8f : 0.55f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    DrawShareButton(dc, g.send, L"Send", true, ShareProblem().empty(), hover_ == kHitShareSend, a);
}

void IslandWindow::DrawShareProgress(ID2D1DeviceContext* dc, const ShareLayout& g, float a) {
    const ShareTransfer& t = shareSnap_.transfer;
    DrawText(dc, (L"Sending to " + t.recipient).c_str(), gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             D2D1::RectF(g.panel.left + 4, g.panel.top + 2, g.secondary.right, g.panel.top + 24), WithAlpha(kText, a));
    const float barTop = g.panel.top + 34, x0 = g.panel.left + 4, x1 = g.panel.right;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * a));
    dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, barTop, x1, barTop + 6), 3, 3}, brush_.Get());
    float fraction = t.total ? static_cast<float>(static_cast<double>(t.done) / static_cast<double>(t.total)) : 0.0f;
    if (t.phase == SharePhase::Inviting) fraction = 1.0f;
    if (fraction > 0) {
        brush_->SetColor(D2D1::ColorF(kAccent.r, kAccent.g, kAccent.b, a));
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, barTop, x0 + std::max(6.0f, (x1 - x0) * std::clamp(fraction, 0.0f, 1.0f)), barTop + 6), 3, 3},
                                 brush_.Get());
    }
    std::wstring line;
    if (t.waiting > 0) {                                              // SafeShare's per-connection limit: it goes on by itself
        const int minutes = (t.waiting + 59) / 60;
        line = L"SafeShare limits how fast one connection can upload. Going on by itself in " +
               (t.waiting < 60 ? std::to_wstring(t.waiting) + L" s." : std::to_wstring(minutes) + (minutes == 1 ? L" minute." : L" minutes."));
    } else if (t.phase == SharePhase::Preparing) {
        line = L"Creating a secure room…";
    } else if (t.phase == SharePhase::Inviting) {
        line = L"Emailing the link…";
    } else {
        wchar_t pct[16];
        swprintf_s(pct, L"%d%%", static_cast<int>(fraction * 100));
        line = L"Encrypting and uploading " + t.current + L" · " + pct;
        if (t.bytesPerSecond > 0) line += L" · " + FormatBytes(static_cast<uint64_t>(t.bytesPerSecond)) + L"/s";
    }
    DrawText(dc, line.c_str(), gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(x0, barTop + 12, g.secondary.right, g.panel.bottom),
             WithAlpha(kTextDim, 1.2f * a), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DrawShareButton(dc, D2D1::RectF(g.primary.left, g.panel.top, g.primary.right, g.panel.top + 28), L"Cancel", false, true,
                    hover_ == kHitShareCancel, a);
}

void IslandWindow::DrawShareResult(ID2D1DeviceContext* dc, const ShareLayout& g, float a) {
    const float cy = g.panel.top + 16;
    brush_->SetColor(D2D1::ColorF(kGreen.r, kGreen.g, kGreen.b, 0.18f * a));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(g.panel.left + 16, cy), 15.0f, 15.0f), brush_.Get());
    DrawText(dc, L"", gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_NORMAL, true), D2D1::RectF(g.panel.left + 1, cy - 15, g.panel.left + 31, cy + 15),
             WithAlpha(kGreen, a), DWRITE_TEXT_ALIGNMENT_CENTER);
    const float tx = g.panel.left + 42;
    DrawText(dc, (L"Sent to " + shareSnap_.transfer.recipient).c_str(), gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             D2D1::RectF(tx, g.panel.top + 2, g.panel.right, g.panel.top + 22), WithAlpha(kText, a));
    DrawText(dc, L"SafeShare emailed them the link. Send the password another way, by text or a call.", gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
             D2D1::RectF(tx, g.panel.top + 22, g.panel.right, g.panel.top + 40), WithAlpha(kTextDim, 1.15f * a));
    DrawShareButton(dc, g.primary, L"Done", true, true, hover_ == kHitShareDone, a);
    DrawShareButton(dc, g.secondary, L"Copy link", false, true, hover_ == kHitShareCopyLink, a);
    DrawShareButton(dc, g.tertiary, L"Copy password", false, !sharePassword_.text.empty(), hover_ == kHitShareCopyPassword, a);
}

void IslandWindow::DrawSharePhone(ID2D1DeviceContext* dc, const ShareLayout& g, float a) {
    const PhoneLink& p = shareSnap_.phone;
    IDWriteTextFormat* title = gfx_.Text(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* body = gfx_.Wrapped(12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    float tx = g.panel.left + 4;
    if ((p.state == PhoneState::Waiting || p.state == PhoneState::Asking) && p.qrSize > 0 &&
        p.qr.size() == static_cast<size_t>(p.qrSize) * static_cast<size_t>(p.qrSize)) {
        const D2D1_RECT_F card = D2D1::RectF(g.panel.left, g.panel.top + 2, g.panel.left + kQrSide + 8, g.panel.top + kQrSide + 10);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, a));                    // a white card with the code (and its quiet zone)
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{card, 8, 8}, brush_.Get());
        const float module = kQrSide / static_cast<float>(p.qrSize + 4);
        const float ox = card.left + 4 + 2 * module, oy = card.top + 4 + 2 * module;
        brush_->SetColor(D2D1::ColorF(0, 0, 0, a));
        for (int y = 0; y < p.qrSize; ++y)
            for (int x = 0; x < p.qrSize; ++x)
                if (p.qr[static_cast<size_t>(y) * static_cast<size_t>(p.qrSize) + static_cast<size_t>(x)])
                    dc->FillRectangle(D2D1::RectF(ox + x * module, oy + y * module, ox + (x + 1) * module + 0.25f, oy + (y + 1) * module + 0.25f),
                                      brush_.Get());
        tx = card.right + 16;
    }
    const D2D1_RECT_F head = D2D1::RectF(tx, g.panel.top + 2, g.panel.right, g.panel.top + 22);
    const D2D1_RECT_F text = D2D1::RectF(tx, g.panel.top + 26, g.panel.right, g.primary.top - 4);
    switch (p.state) {
    case PhoneState::Off:
        DrawText(dc, L"Your phone, on the same Wi-Fi", title, head, WithAlpha(kText, a));
        DrawText(dc, L"Your phone can download the files above, and send files to this PC (they land on the Shelf). Nothing goes "
                     L"through the internet, and no app is needed.",
                 body, text, WithAlpha(kTextDim, 1.15f * a), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DrawShareButton(dc, g.primary, L"Connect", true, static_cast<bool>(share_.phone), hover_ == kHitSharePhoneStart, a);
        break;
    case PhoneState::Starting:
        DrawText(dc, L"Getting ready…", title, head, WithAlpha(kText, a));
        DrawShareButton(dc, g.primary, L"Stop", false, true, hover_ == kHitSharePhoneStop, a);
        break;
    case PhoneState::Waiting:
        DrawText(dc, L"Scan this with your phone's camera", title, head, WithAlpha(kText, a));
        DrawText(dc, (L"The phone must be on the same Wi-Fi. If Windows asks, allow Eflow on private networks. It stops by itself "
                      L"after 10 minutes without use. Or open " + p.url).c_str(),
                 body, text, WithAlpha(kTextDim, 1.15f * a), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DrawShareButton(dc, g.primary, L"Stop", false, true, hover_ == kHitSharePhoneStop, a);
        break;
    case PhoneState::Asking:
        DrawText(dc, L"A phone wants to connect", title, head, WithAlpha(kAmber, a));
        DrawText(dc, (p.device + L". Allow it only if it is your phone.").c_str(), body, text, WithAlpha(kTextDim, 1.15f * a),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DrawShareButton(dc, g.primary, L"Allow", true, true, hover_ == kHitSharePhoneAllow, a);
        DrawShareButton(dc, g.secondary, L"Deny", false, true, hover_ == kHitSharePhoneDeny, a);
        break;
    case PhoneState::Connected: {
        DrawText(dc, (L"Connected · " + p.device).c_str(), title, head, WithAlpha(kGreen, a));
        std::wstring line = shareFiles_.empty() ? L"Drop files above to send them to the phone." : L"The phone can download the files above.";
        line += L" Files sent from the phone land on the Shelf";
        line += p.received ? L" (" + std::to_wstring(p.received) + L" so far)." : L".";
        DrawText(dc, line.c_str(), body, text, WithAlpha(kTextDim, 1.15f * a), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DrawShareButton(dc, g.primary, L"Disconnect", false, true, hover_ == kHitSharePhoneStop, a);
        break;
    }
    case PhoneState::Unavailable:
        DrawText(dc, L"The phone link can't start", title, head, WithAlpha(kAmber, a));
        DrawText(dc, p.reason.c_str(), body, text, WithAlpha(kTextDim, 1.15f * a), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DrawShareButton(dc, g.primary, L"Try again", !p.publicNetwork, true, hover_ == kHitSharePhoneStart, a);
        if (p.publicNetwork) DrawShareButton(dc, g.secondary, L"It's my Wi-Fi", true, true, hover_ == kHitSharePhoneAnyway, a);
        break;
    }
}

void IslandWindow::DrawShareSent(ID2D1DeviceContext* dc, const ShareLayout& g, float a) {
    const auto& sent = shareSnap_.sent;
    if (sent.empty()) {
        const float mid = (g.list.top + g.list.bottom) / 2;
        DrawText(dc, L"Nothing sent in the last 10 hours", gfx_.Text(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(g.list.left, mid - 12, g.list.right, mid + 12),
                 WithAlpha(kTextDim, 1.2f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    shareScroll_ = std::clamp(shareScroll_, 0, std::max(0, static_cast<int>(sent.size()) - g.rows));
    const int64_t now = UnixNow();
    IDWriteTextFormat* title = gfx_.Text(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* caption = gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* icons = gfx_.Text(12.0f, DWRITE_FONT_WEIGHT_NORMAL, true);
    dc->PushAxisAlignedClip(g.list, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int row = 0; row < g.rows; ++row) {
        const size_t i = static_cast<size_t>(shareScroll_ + row);
        if (i >= sent.size()) break;
        const SentShare& s = sent[i];
        const float top = g.list.top + row * kRowH, cy = top + kRowH / 2;
        const bool downloaded = s.downloads > 0, opened = s.opens > 0;
        const D2D1_COLOR_F tint = downloaded ? kGreen : opened ? kAmber : kAccent;
        brush_->SetColor(D2D1::ColorF(tint.r, tint.g, tint.b, 0.16f * a));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(g.list.left + 15, cy), 15.0f, 15.0f), brush_.Get());
        DrawText(dc, downloaded ? L"" : opened ? L"" : L"", icons, D2D1::RectF(g.list.left, cy - 15, g.list.left + 30, cy + 15),
                 WithAlpha(tint, a), DWRITE_TEXT_ALIGNMENT_CENTER);
        const float textLeft = g.list.left + 40, right = g.list.right - 66;
        const std::wstring left = FormatLeft(s.expiresAt, now);
        const float leftW = std::ceil(gfx_.MeasureWidth(left.c_str(), caption));
        DrawText(dc, s.title.c_str(), title, D2D1::RectF(textLeft, top + 5, right - leftW - 12, top + 24), WithAlpha(kText, a));
        DrawText(dc, left.c_str(), caption, D2D1::RectF(right - leftW, top + 5, right, top + 24), WithAlpha(kTextDim, a), DWRITE_TEXT_ALIGNMENT_TRAILING);
        std::wstring second = L"to " + s.recipient + L" · ";
        if (downloaded) second += L"Downloaded" + (s.downloads > 1 ? L" " + std::to_wstring(s.downloads) + L" times" : std::wstring());
        else if (opened) second += L"Opened" + (s.opens > 1 ? L" on " + std::to_wstring(s.opens) + L" devices" : std::wstring());
        else second += L"Not opened yet";
        if (!s.lastSeen.empty()) second += L" · " + s.lastSeen;
        DrawText(dc, second.c_str(), caption, D2D1::RectF(textLeft, top + 23, right, top + 40), WithAlpha(kTextDim, 1.1f * a));

        const D2D1_RECT_F copy = D2D1::RectF(g.list.right - 60, cy - 13, g.list.right - 34, cy + 13);
        const D2D1_RECT_F remove = D2D1::RectF(g.list.right - 28, cy - 13, g.list.right - 2, cy + 13);
        const bool copyHot = hover_ == kHitShareRowCopy + row, removeHot = hover_ == kHitShareRowDelete + row;
        const bool confirming = shareConfirm_ == s.roomId && GetTickCount64() < shareConfirmUntil_;
        DrawText(dc, L"", icons, copy, WithAlpha(copyHot ? kText : kTextDim, (copyHot ? 1.0f : 0.8f) * a), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (confirming) {
            brush_->SetColor(D2D1::ColorF(kRed.r, kRed.g, kRed.b, 0.22f * a));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((remove.left + remove.right) / 2, cy), 13.0f, 13.0f), brush_.Get());
        }
        DrawText(dc, L"", icons, remove, WithAlpha(removeHot || confirming ? kRed : kTextDim, (removeHot || confirming ? 1.0f : 0.8f) * a),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    dc->PopAxisAlignedClip();
    const bool confirming = !shareConfirm_.empty() && GetTickCount64() < shareConfirmUntil_;
    const int hidden = static_cast<int>(sent.size()) - shareScroll_ - g.rows;
    if (confirming || hidden > 0)
        DrawText(dc, confirming ? L"Press the bin again to close the room for everyone" : (L"+" + std::to_wstring(hidden) + L" more").c_str(),
                 caption, D2D1::RectF(g.list.left, g.list.bottom + 2, g.list.right, g.list.bottom + 16), WithAlpha(confirming ? kRed : kTextDim, a),
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void IslandWindow::DrawShareStatus(ID2D1DeviceContext* dc, const ShareLayout& g, float a) {
    const ShareTransfer& t = shareSnap_.transfer;
    std::wstring text;
    D2D1_COLOR_F color = kTextDim;
    float strength = 1.1f;                                            // the dim hint a little stronger; warnings as they are
    if (GetTickCount64() < shareNoteUntil_ && !shareNote_.empty()) {
        text = shareNote_, color = shareNoteGood_ ? kGreen : kAmber, strength = 1.0f;
    } else if (t.phase == SharePhase::Failed && !t.error.empty()) {
        text = t.error, color = kRed, strength = 1.0f;
    } else if (t.phase == SharePhase::Cancelled) {
        text = L"Cancelled. Nothing was sent.";
    } else if (ShareBusy() || t.phase == SharePhase::Sent) {
        text = L"The files are encrypted on this PC before they are uploaded. The room closes by itself after 10 hours.";
    } else if (hover_ >= kHitShareDevices && hover_ < kHitShareDevices + 3) {
        const int n = kDeviceChoices[hover_ - kHitShareDevices];
        text = n == 1 ? L"Only the first device that opens it gets in. If they forward the link and password, it says \"Room is full\"."
                      : L"Up to " + std::to_wstring(n) + L" devices can open it, so a forwarded link works until they are used up.";
    } else if (hover_ == kHitShareGenerate) {
        text = L"Make a strong password (it is shown so you can send it to them)";
    } else if (hover_ == kHitShareSend && !ShareProblem().empty()) {
        text = ShareProblem(), color = kAmber, strength = 1.0f;
    } else {
        text = L"The room lasts 10 hours. Send the password another way, not in the same email.";
    }
    DrawText(dc, text.c_str(), gfx_.Text(11.5f, DWRITE_FONT_WEIGHT_NORMAL), g.status, WithAlpha(color, strength * a));
}

// ---- input ---------------------------------------------------------------------------------------------------------------

int IslandWindow::SafeShareHit(D2D1_POINT_2F p, const D2D1_RECT_F& island) const {
    const ShareLayout g = ShareGeometry(island);
    if (shareSentView_) {
        if (In(g.back, p)) return kHitShareBack;
        if (!In(g.list, p)) return -1;
        const int row = static_cast<int>((p.y - g.list.top) / kRowH);
        if (row >= g.rows || shareScroll_ + row >= static_cast<int>(shareSnap_.sent.size())) return -1;
        if (p.x >= g.list.right - 60 && p.x < g.list.right - 31) return kHitShareRowCopy + row;
        if (p.x >= g.list.right - 31) return kHitShareRowDelete + row;
        return -1;
    }
    if (In(g.modeEmail, p)) return kHitShareModeEmail;
    if (In(g.modePhone, p)) return kHitShareModePhone;
    if (!shareSnap_.sent.empty() && In(g.sentList, p)) return kHitShareSentList;
    const bool busy = ShareBusy();
    if (In(g.files, p)) {
        if (busy) return -1;
        if (shareFiles_.empty()) return kHitShareAddFiles;
        const auto chips = ShareChips(g);
        for (size_t c = 0; c < chips.size(); ++c)
            if (chips[c].file >= 0 && In(chips[c].close, p)) return kHitShareFileRemove + static_cast<int>(c);
        if (In(g.clear, p)) return kHitShareClearFiles;
        return kHitShareAddFiles;                                     // anywhere else in the area: add more
    }
    if (sharePhone_) {
        const PhoneState s = shareSnap_.phone.state;
        if (In(g.primary, p)) {
            if (s == PhoneState::Off || s == PhoneState::Unavailable) return kHitSharePhoneStart;
            return s == PhoneState::Asking ? kHitSharePhoneAllow : kHitSharePhoneStop;
        }
        if (s == PhoneState::Asking && In(g.secondary, p)) return kHitSharePhoneDeny;
        if (s == PhoneState::Unavailable && shareSnap_.phone.publicNetwork && In(g.secondary, p)) return kHitSharePhoneAnyway;
        return -1;
    }
    if (busy) return In(D2D1::RectF(g.primary.left, g.panel.top, g.primary.right, g.panel.top + 28), p) ? kHitShareCancel : -1;
    if (shareSnap_.transfer.phase == SharePhase::Sent) {
        if (In(g.primary, p)) return kHitShareDone;
        if (In(g.secondary, p)) return kHitShareCopyLink;
        if (In(g.tertiary, p)) return kHitShareCopyPassword;
        return -1;
    }
    if (In(g.reveal, p)) return kHitShareReveal;
    if (In(g.generate, p)) return kHitShareGenerate;
    if (In(g.name, p)) return kHitShareName;
    if (In(g.email, p)) return kHitShareEmail;
    if (In(g.password, p)) return kHitSharePassword;
    for (int i = 0; i < 3; ++i)
        if (In(g.devices[static_cast<size_t>(i)], p)) return kHitShareDevices + i;
    if (In(g.send, p)) return kHitShareSend;
    return -1;
}

void IslandWindow::SafeShareClick(int id) {
    if (id == kHitShareModeEmail || id == kHitShareModePhone) {
        sharePhone_ = id == kHitShareModePhone;
        if (sharePhone_ && FieldSection(field_) == static_cast<int>(Section::SafeShare)) Blur(false);
        OfferShareFiles();
    } else if (id == kHitShareSentList || id == kHitShareBack) {
        shareSentView_ = id == kHitShareSentList;
        shareScroll_ = 0;
        shareConfirm_.clear();
        if (shareSentView_ && FieldSection(field_) == static_cast<int>(Section::SafeShare)) Blur(false);
    } else if (id == kHitShareAddFiles) {
        ChooseShareFiles();
    } else if (id == kHitShareClearFiles) {
        shareFiles_.clear();
        OfferShareFiles();
    } else if (id >= kHitShareFileRemove && id < kHitShareRowCopy) {
        const auto chips = ShareChips(ShareGeometry(IslandRect()));
        const size_t c = static_cast<size_t>(id - kHitShareFileRemove);
        if (c < chips.size() && chips[c].file >= 0) {
            shareFiles_.erase(shareFiles_.begin() + chips[c].file);
            OfferShareFiles();
        }
    } else if (id == kHitShareName) {
        Focus(shareName_);
    } else if (id == kHitShareEmail) {
        Focus(shareEmail_);
    } else if (id == kHitSharePassword) {
        Focus(sharePassword_);
    } else if (id == kHitShareReveal) {
        shareReveal_ = !shareReveal_;
    } else if (id == kHitShareGenerate) {
        const std::wstring password = sharecrypto::MakePassword();
        if (password.empty()) {
            ShareSay(L"Couldn't make a password. Type one instead.");
        } else {
            sharePassword_.text = password;
            sharePassword_.caret = password.size();
            shareReveal_ = true;                                      // you need to read it to tell them
        }
    } else if (id >= kHitShareDevices && id < kHitShareDevices + 3) {
        shareDevices_ = kDeviceChoices[id - kHitShareDevices];
    } else if (id == kHitShareSend) {
        SendShare();
    } else if (id == kHitShareCancel) {
        if (share_.cancel) share_.cancel();
    } else if (id == kHitShareDone) {
        ResetShareForm();
    } else if (id == kHitShareCopyPassword) {
        const bool copied = CopyText(hwnd_, sharePassword_.text, true);
        ShareSay(copied ? L"Password copied. It is left out of clipboard history." : L"Couldn't copy: another app is using the clipboard.", copied);
    } else if (id == kHitShareCopyLink) {
        CopyShareLink(shareSnap_.transfer.roomId);
    } else if (id == kHitSharePhoneStart || id == kHitSharePhoneStop) {
        if (share_.phone) share_.phone(id == kHitSharePhoneStart);
        if (id == kHitSharePhoneStart) OfferShareFiles();
    } else if (id == kHitSharePhoneAnyway) {
        if (share_.phoneAnyway) share_.phoneAnyway();
        OfferShareFiles();
    } else if (id == kHitSharePhoneAllow || id == kHitSharePhoneDeny) {
        if (share_.phoneAllow) share_.phoneAllow(id == kHitSharePhoneAllow);
    } else if (id >= kHitShareRowCopy && id <= kHitShareLast) {
        const bool remove = id >= kHitShareRowDelete;
        const size_t i = static_cast<size_t>(shareScroll_ + id - (remove ? kHitShareRowDelete : kHitShareRowCopy));
        if (i >= shareSnap_.sent.size()) return;
        const std::wstring& room = shareSnap_.sent[i].roomId;
        if (!remove) {
            CopyShareLink(room);
        } else if (shareConfirm_ == room && GetTickCount64() < shareConfirmUntil_) {
            shareConfirm_.clear();
            if (share_.remove) share_.remove(room);
        } else {                                                      // closing a room is final: press twice
            shareConfirm_ = room;
            shareConfirmUntil_ = GetTickCount64() + kConfirmMs;
            SetTimer(hwnd_, kShareTimer, static_cast<UINT>(kConfirmMs + 50), nullptr);
        }
    }
}

bool IslandWindow::SafeShareKey(WPARAM vk) {
    TextField* fields[] = {&shareName_, &shareEmail_, &sharePassword_};
    const auto at = std::find(std::begin(fields), std::end(fields), field_);
    if (at == std::end(fields)) return false;
    if (vk == VK_RETURN) {
        SendShare();
    } else if (vk == VK_TAB || vk == VK_DOWN || vk == VK_UP) {        // to the next box (Shift+Tab / Up: the one before)
        const bool back = vk == VK_UP || (vk == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000));
        const size_t i = static_cast<size_t>(at - std::begin(fields));
        Focus(*fields[(i + (back ? 2 : 1)) % 3]);
    } else {
        return false;
    }
    return true;
}

// ---- actions -------------------------------------------------------------------------------------------------------------

void IslandWindow::SetSafeShare(const SafeShareSnapshot& snapshot) {
    const PhoneState phoneWas = shareSnap_.phone.state;
    const SharePhase phaseWas = shareSnap_.transfer.phase;
    std::wstring news;                                                // a sent room was just opened or downloaded
    for (const SentShare& now : snapshot.sent)
        for (const SentShare& was : shareSnap_.sent)
            if (now.roomId == was.roomId && (now.opens > was.opens || now.downloads > was.downloads))
                news = now.title + (now.downloads > was.downloads ? L" was downloaded" : L" was opened") +
                       (now.lastSeen.empty() ? L"." : L" \u00B7 " + now.lastSeen);
    shareSnap_ = snapshot;
    if (!shareSeeded_) {                                              // the name and the choice from the last time
        shareSeeded_ = true;
        if (shareName_.text.empty()) shareName_.text = snapshot.sender, shareName_.caret = shareName_.text.size();
        if (std::find(std::begin(kDeviceChoices), std::end(kDeviceChoices), snapshot.maxDevices) != std::end(kDeviceChoices))
            shareDevices_ = snapshot.maxDevices;
    }
    const bool onTab = expanded_ && section_ == Section::SafeShare;
    if (snapshot.phone.state == PhoneState::Asking && phoneWas != PhoneState::Asking) {
        if (Present(Section::SafeShare, true)) {                      // someone is knocking: the user decides now
            sharePhone_ = true;
            shareSentView_ = false;
            Expand();
        }
    }
    const SharePhase phase = snapshot.transfer.phase;
    if (phase != phaseWas && (phase == SharePhase::Sent || phase == SharePhase::Failed) && !onTab)
        tabDot_[static_cast<size_t>(Section::SafeShare)] = phase == SharePhase::Failed ? 2 : 1;
    if (!news.empty()) {
        if (onTab) ShareSay(news, true);                              // on the tab: said right there
        else tabDot_[static_cast<size_t>(Section::SafeShare)] = std::max(tabDot_[static_cast<size_t>(Section::SafeShare)], 1);
    }
    if (onTab) Animate();
}

void IslandWindow::AddShareFiles(const std::vector<std::wstring>& paths) {
    if (shareSnap_.transfer.phase == SharePhase::Sent) ResetShareForm();   // new files after a send: a fresh start
    uint64_t total = 0;
    for (const ShareFile& f : shareFiles_) total += f.size;
    std::wstring note;
    for (const std::wstring& path : paths) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            note = L"Couldn't open " + FileName(path) + L".";
            continue;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            note = L"Folders can't be sent. Zip it first: right-click, then Compress to ZIP file.";
            continue;
        }
        if (std::any_of(shareFiles_.begin(), shareFiles_.end(), [&](const ShareFile& f) { return SamePath(f.path, path); })) continue;
        const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (shareFiles_.size() >= kShareMaxFiles) {
            note = L"A room holds up to 100 files.";
            break;
        }
        if (total + size > kShareMaxBytes) {
            note = L"A room holds up to 4 GB. " + FileName(path) + L" didn't fit.";
            continue;
        }
        total += size;
        shareFiles_.push_back({path, FileName(path), size});
    }
    if (!note.empty()) ShareSay(note);
    OfferShareFiles();
    Animate();
}

void IslandWindow::ChooseShareFiles() {
    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Choose files to send with SafeShare";
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> paths;
    const std::wstring first = buffer.data();
    const wchar_t* next = buffer.data() + first.size() + 1;
    if (*next == L'\0') {
        paths.push_back(first);                                       // one file: its full path
    } else {
        for (; *next; next += wcslen(next) + 1) paths.push_back(first + L"\\" + next);
    }
    AddShareFiles(paths);
}

bool IslandWindow::ShareBusy() const {
    const SharePhase p = shareSnap_.transfer.phase;
    return p == SharePhase::Preparing || p == SharePhase::Uploading || p == SharePhase::Inviting;
}

std::wstring IslandWindow::ShareProblem() const {
    if (ShareBusy()) return L"A send is already on its way.";
    if (!share_.send) return L"SafeShare isn't available right now.";
    if (shareFiles_.empty()) return L"Add the files to send first.";
    if (Trimmed(shareName_.text).empty()) return L"Type your name. The email says who it is from.";
    const std::wstring email = Trimmed(shareEmail_.text);
    if (email.empty()) return L"Type the recipient's email.";
    if (!IsPlausibleEmail(email)) return L"That email address doesn't look right.";
    const std::wstring& pw = sharePassword_.text;
    if (pw.size() < kShareMinPassword) return L"The password needs at least 8 characters.";
    if (pw.front() == L' ' || pw.back() == L' ') return L"The password can't start or end with a space.";
    return {};
}

void IslandWindow::SendShare() {
    if (const std::wstring problem = ShareProblem(); !problem.empty()) {
        ShareSay(problem);
        Animate();
        return;
    }
    ShareSendRequest request;
    for (const ShareFile& f : shareFiles_) request.paths.push_back(f.path);
    request.sender = Trimmed(shareName_.text);
    request.email = Trimmed(shareEmail_.text);
    request.password = sharePassword_.text;
    request.maxDevices = shareDevices_;
    shareNoteUntil_ = 0;
    if (FieldSection(field_) == static_cast<int>(Section::SafeShare)) Blur(false);
    share_.send(request);
    Animate();
}

// After a send: the files, the recipient and the password go; your name and the device choice stay.
void IslandWindow::ResetShareForm() {
    shareFiles_.clear();
    shareEmail_.Clear();
    sharePassword_.Clear();
    shareReveal_ = false;
    if (share_.done) share_.done();
    OfferShareFiles();
}

void IslandWindow::CopyShareLink(const std::wstring& roomId) {
    const bool copied = share_.copyLink && share_.copyLink(roomId);
    ShareSay(copied ? L"Link copied. It opens the files, so share it only with them." : L"Couldn't copy the link: another app is using the clipboard.",
             copied);
}

void IslandWindow::ShareSay(const std::wstring& note, bool good) {
    shareNote_ = note;
    shareNoteGood_ = good;
    shareNoteUntil_ = GetTickCount64() + kNoteMs;
    SetTimer(hwnd_, kShareTimer, static_cast<UINT>(kNoteMs + 50), nullptr);   // (the note fades back to the hint)
}

void IslandWindow::OfferShareFiles() {
    if (!share_.offer) return;
    std::vector<std::wstring> paths;
    for (const ShareFile& f : shareFiles_) paths.push_back(f.path);
    share_.offer(paths);
}

} // namespace eflow
