#pragma once
#include <cstdint>
#include <string>

namespace eflow {

// "Codeforces contest 4 March 8pm, 15 min before" -> what, when, how often, how early. Pure functions (no Windows UI),
// so they are unit tested on their own (tests/when_test.cpp). Times are seconds since 1970 (UTC); the parsing and the
// formatting use the PC's local time zone.

enum class Repeat { None, Daily, Weekdays, Weekly };

struct When {
    bool ok = false;               // a date, a time, "in 20 minutes" or "every day" was found
    std::wstring title;            // what is left of the text: "Codeforces contest"
    int64_t at = 0;                // when it first rings
    Repeat repeat = Repeat::None;
    int earlyMinutes = 0;          // "15 min before": a heads-up that long before (0 = none)
    bool meridiemGuessed = false;  // "at 8" had no am / pm: one was chosen, and the user can flip it
};

When ParseWhen(const std::wstring& text, int64_t now);
int64_t FlipMeridiem(int64_t at);                                // 8:00 AM <-> 8:00 PM, same day
int64_t NextOccurrence(int64_t at, Repeat repeat, int64_t after); // the first ring after `after` (repeating reminders)
std::wstring FormatWhen(int64_t at, Repeat repeat, int64_t now);  // "Wed, Mar 4 · 8:00 PM", "Tomorrow · 7:00 AM", "Daily · 7:00 AM"
std::wstring FormatCountdown(int64_t at, int64_t now);           // "in 3 days", "in 2 h 5 min", "in 40 s", "now"
std::wstring FormatEarly(int minutes);                           // "15 min before", "1 h before", "1 day before"
int64_t UnixNow();

} // namespace eflow
