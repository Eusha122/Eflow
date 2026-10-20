#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "core/Image.h"

namespace eflow {

// Something going on right now that the thin bar shows as a small live pill (like an iPhone Live Activity):
// a download or file copy, Solu's research, the focus timer, one of Solu's rare nudges, or something just plugged in.
struct Activity {
    enum class Kind { Progress, Timer, Nudge, Done, Device, Screenshot };
    enum class Mark { Close, Eject, Save };   // the mark at its right end (when dismissible): what a middle click on it does
    std::wstring id;                 // stable while it runs ("download:C:\...", "focus", "research", "nudge:3")
    Kind kind = Kind::Progress;
    std::wstring glyph = L"\uE896";  // Segoe Fluent Icons
    std::wstring title;              // "Downloading", "Focus", "Researching"
    std::wstring detail;             // "setup.exe · 340 MB", "history of chess"
    float progress = -1.0f;          // 0..1, or -1 when unknown (an indeterminate shimmer)
    uint64_t endsAt = 0;             // Timer: GetTickCount64() when it ends
    uint64_t length = 0;             // Timer: total milliseconds (for the ring)
    uint32_t accent = 0x0A84FF;      // 0xRRGGBB
    bool dismissible = false;        // it has a mark at its right end (nudges, finished items, drives, screenshots)
    Mark mark = Mark::Close;
    uint64_t expiresAt = 0;          // removed by itself at this tick (0 = when its source ends it)
    uint64_t startedAt = 0;          // GetTickCount64() when it appeared: the arrival animation plays from here (0 = none)
    std::wstring open;               // what a middle click on the pill opens (a drive's root) instead of the island
    std::shared_ptr<const ImageBGRA> thumb;   // a picture in place of the icon (a screenshot)
};

} // namespace eflow
