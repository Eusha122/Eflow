#pragma once
#include <cstdint>
#include <string>

namespace eflow {

// Something going on right now that the thin bar shows as a small live pill (like an iPhone Live Activity):
// a download or file copy, Solu's research, the focus timer, or one of Solu's rare nudges.
struct Activity {
    enum class Kind { Progress, Timer, Nudge, Done };
    std::wstring id;                 // stable while it runs ("download:C:\...", "focus", "research", "nudge:3")
    Kind kind = Kind::Progress;
    std::wstring glyph = L"\uE896";  // Segoe Fluent Icons
    std::wstring title;              // "Downloading", "Focus", "Researching"
    std::wstring detail;             // "setup.exe · 340 MB", "history of chess"
    float progress = -1.0f;          // 0..1, or -1 when unknown (an indeterminate shimmer)
    uint64_t endsAt = 0;             // Timer: GetTickCount64() when it ends
    uint64_t length = 0;             // Timer: total milliseconds (for the ring)
    uint32_t accent = 0x0A84FF;      // 0xRRGGBB
    bool dismissible = false;        // nudges and finished items have a close button
    uint64_t expiresAt = 0;          // removed by itself at this tick (0 = when its source ends it)
};

} // namespace eflow
