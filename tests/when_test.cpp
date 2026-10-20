// Unit tests for core/When (the reminder time parser). Built as eflow-tests; run build\debug\eflow-tests.exe.
// "Now" is fixed at Monday 19 October 2026, 8:15 PM local time, so the expectations never depend on the day they run.
#include <cstdio>
#include <ctime>
#include <string>

#include "core/When.h"

using eflow::Repeat;

int LoginCodeTests();   // tests/login_code_test.cpp

namespace {

int failed = 0;

int64_t Local(int year, int month, int day, int hour, int minute) {
    std::tm tm{};
    tm.tm_year = year - 1900, tm.tm_mon = month - 1, tm.tm_mday = day, tm.tm_hour = hour, tm.tm_min = minute, tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm));
}

const int64_t kNow = Local(2026, 10, 19, 20, 15);

void Expect(const wchar_t* text, const wchar_t* title, int64_t at, Repeat repeat = Repeat::None, int early = 0, bool guessed = false) {
    const eflow::When w = eflow::ParseWhen(text, kNow);
    const bool ok = w.ok && w.title == title && w.at == at && w.repeat == repeat && w.earlyMinutes == early && w.meridiemGuessed == guessed;
    if (!ok) {
        ++failed;
        std::wprintf(L"[FAIL] \"%s\"\n       got:  ok=%d title=\"%s\" at=%s repeat=%d early=%d guessed=%d\n       want: title=\"%s\" at=%s repeat=%d early=%d guessed=%d\n",
                     text, w.ok, w.title.c_str(), eflow::FormatWhen(w.at, w.repeat, kNow).c_str(), static_cast<int>(w.repeat), w.earlyMinutes,
                     w.meridiemGuessed, title, eflow::FormatWhen(at, repeat, kNow).c_str(), static_cast<int>(repeat), early, guessed);
    } else {
        std::wprintf(L"[PASS] \"%s\" -> %s: %s\n", text, eflow::FormatWhen(w.at, w.repeat, kNow).c_str(), w.title.c_str());
    }
}

void ExpectNoTime(const wchar_t* text) {
    const eflow::When w = eflow::ParseWhen(text, kNow);
    if (w.ok) {
        ++failed;
        std::wprintf(L"[FAIL] \"%s\" should have no time, got %s\n", text, eflow::FormatWhen(w.at, w.repeat, kNow).c_str());
    } else {
        std::wprintf(L"[PASS] \"%s\" -> no time\n", text);
    }
}

}  // namespace

int main() {
    // the user's own example, and its spellings
    Expect(L"Codeforces contest 4 March 8pm", L"Codeforces contest", Local(2027, 3, 4, 20, 0));
    Expect(L"codeforces contest at 4 march at 8", L"Codeforces contest", Local(2027, 3, 4, 8, 0), Repeat::None, 0, true);
    Expect(L"Codeforces contest on March 4th at 8:35 pm", L"Codeforces contest", Local(2027, 3, 4, 20, 35));
    Expect(L"Codeforces Div 2 4/3 20:35", L"Codeforces Div 2", Local(2027, 3, 4, 20, 35));
    Expect(L"Codeforces contest 4 march 8pm, 15 min before", L"Codeforces contest", Local(2027, 3, 4, 20, 0), Repeat::None, 15);
    // later this year
    Expect(L"Exam 12 December 10am", L"Exam", Local(2026, 12, 12, 10, 0));
    Expect(L"dentist on nov 3 at 4:30", L"Dentist", Local(2026, 11, 3, 16, 30), Repeat::None, 0, true);
    // today, tonight, tomorrow, weekdays
    Expect(L"call mom at 9pm", L"Call mom", Local(2026, 10, 19, 21, 0));
    Expect(L"call mom at 9", L"Call mom", Local(2026, 10, 19, 21, 0), Repeat::None, 0, true);     // 9 am has passed: 9 pm
    Expect(L"take the pizza out at 8:30", L"Take the pizza out", Local(2026, 10, 19, 20, 30), Repeat::None, 0, true);
    Expect(L"submit report tomorrow at 10am", L"Submit report", Local(2026, 10, 20, 10, 0));
    Expect(L"meeting tomorrow", L"Meeting", Local(2026, 10, 20, 9, 0));
    Expect(L"movie tonight at 10", L"Movie", Local(2026, 10, 19, 22, 0), Repeat::None, 0, true);
    Expect(L"football friday 5pm", L"Football", Local(2026, 10, 23, 17, 0));
    Expect(L"standup next monday 10:00", L"Standup", Local(2026, 10, 26, 10, 0), Repeat::None, 0, true);   // 10:00 could be 10 pm
    Expect(L"standup next monday 22:00", L"Standup", Local(2026, 10, 26, 22, 0));
    Expect(L"buy milk at noon", L"Buy milk", Local(2026, 10, 20, 12, 0));                         // noon today has passed
    // in ...
    Expect(L"tea in 5 minutes", L"Tea", kNow + 5 * 60);
    Expect(L"check the oven in half an hour", L"Check the oven", kNow + 30 * 60);
    Expect(L"remind me to stretch in 2 hours", L"Stretch", kNow + 2 * 3600);
    // repeating
    Expect(L"gym every day at 7am", L"Gym", Local(2026, 10, 20, 7, 0), Repeat::Daily);
    Expect(L"water the plants every morning", L"Water the plants", Local(2026, 10, 20, 9, 0), Repeat::Daily);
    Expect(L"standup weekdays at 10", L"Standup", Local(2026, 10, 20, 10, 0), Repeat::Weekdays, 0, true);
    Expect(L"team call every friday at 6pm", L"Team call", Local(2026, 10, 23, 18, 0), Repeat::Weekly);
    // alarms
    Expect(L"alarm at 6:30am", L"Alarm", Local(2026, 10, 20, 6, 30));
    Expect(L"set an alarm for 7", L"Alarm", Local(2026, 10, 20, 7, 0), Repeat::None, 0, true);
    // no time: the title alone, nothing to ring
    ExpectNoTime(L"Codeforces contest");
    ExpectNoTime(L"I may go");
    ExpectNoTime(L"Read 45 pages");

    // helpers
    const int64_t flipped = eflow::FlipMeridiem(Local(2027, 3, 4, 8, 0));
    if (flipped != Local(2027, 3, 4, 20, 0)) ++failed, std::wprintf(L"[FAIL] FlipMeridiem\n");
    if (eflow::FormatCountdown(kNow + 3 * 86400, kNow) != L"in 3 days") ++failed, std::wprintf(L"[FAIL] FormatCountdown days\n");
    if (eflow::FormatEarly(15) != L"15 min before" || eflow::FormatEarly(60) != L"1 h before") ++failed, std::wprintf(L"[FAIL] FormatEarly\n");

    failed += LoginCodeTests();
    std::wprintf(failed ? L"\n%d failed\n" : L"\nall passed\n", failed);
    return failed ? 1 : 0;
}
