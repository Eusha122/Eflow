#include "core/When.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <cwctype>
#include <regex>
#include <vector>

namespace eflow {
namespace {

constexpr int64_t kMinute = 60, kHour = 3600, kDay = 86400;

std::tm Local(int64_t t) {
    std::tm tm{};
    const std::time_t tt = static_cast<std::time_t>(t);
    localtime_s(&tm, &tt);
    return tm;
}

int64_t FromLocal(std::tm tm) {                   // mktime normalises day 32, hour 25, ... and handles summer time
    tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm));
}

int64_t At(const std::tm& day, int hour, int minute) {
    std::tm tm = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    return FromLocal(tm);
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

int Month(const std::wstring& word) {             // "mar" / "march" -> 2 (0-based), -1 if not a month
    static const std::array<const wchar_t*, 12> names = {L"jan", L"feb", L"mar", L"apr", L"may", L"jun",
                                                         L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};
    for (int i = 0; i < 12; ++i)
        if (word.rfind(names[i], 0) == 0) return i;
    return -1;
}

int Weekday(const std::wstring& word) {           // "fri" / "friday" -> 5 (Sunday = 0), -1 if not a day
    static const std::array<const wchar_t*, 7> names = {L"sun", L"mon", L"tue", L"wed", L"thu", L"fri", L"sat"};
    for (int i = 0; i < 7; ++i)
        if (word.rfind(names[i], 0) == 0) return i;
    return -1;
}

int Number(const std::wstring& word) {            // "20", "a", "an", "one" ... "twelve"
    static const std::array<const wchar_t*, 13> words = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six",
                                                         L"seven", L"eight", L"nine", L"ten", L"eleven", L"twelve"};
    if (word == L"a" || word == L"an") return 1;
    for (int i = 0; i < 13; ++i)
        if (word == words[i]) return i;
    return word.empty() || !std::iswdigit(word[0]) ? -1 : std::stoi(word);
}

int64_t UnitSeconds(const std::wstring& unit) {
    if (unit[0] == L'm') return kMinute;
    if (unit[0] == L'h') return kHour;
    if (unit[0] == L'd') return kDay;
    return 7 * kDay;                               // weeks
}

// The parts of the text that were understood; they are cut out of the title afterwards.
struct Spans {
    std::wstring& lower;
    std::vector<std::pair<size_t, size_t>> cut;
    bool Search(const std::wregex& re, std::wsmatch& m) {
        std::wstring::const_iterator from = lower.cbegin();
        while (std::regex_search(from, lower.cend(), m, re)) {
            const size_t start = static_cast<size_t>(m[0].first - lower.cbegin()), end = start + m.length(0);
            const bool taken = std::any_of(cut.begin(), cut.end(), [&](auto& c) { return start < c.second && end > c.first; });
            if (!taken) {
                cut.emplace_back(start, end);
                return true;
            }
            from = m[0].second;
        }
        return false;
    }
};

const wchar_t kTimeWord[] = L"(?:am|pm|a\\.m\\.|p\\.m\\.)";

}  // namespace

int64_t UnixNow() { return static_cast<int64_t>(std::time(nullptr)); }

When ParseWhen(const std::wstring& text, int64_t now) {
    When w;
    std::wstring lower = Lower(text);
    Spans spans{lower, {}};
    std::wsmatch m;
    const std::tm today = Local(now);

    // ---- how early, and how often --------------------------------------------------------------------
    static const std::wregex early(L"\\b(\\d+|a|an|one|two|three|half an)\\s*(minutes?|mins?|m|hours?|hrs?|h|days?)\\s+(?:before|early|earlier|ahead)\\b");
    if (spans.Search(early, m)) {
        const std::wstring n = m[1].str();
        w.earlyMinutes = n == L"half an" ? 30 : Number(n) * static_cast<int>(UnitSeconds(m[2].str()) / kMinute);
    }
    static const std::wregex every(L"\\b(?:every\\s*day|everyday|daily|each day|every (?:morning|evening|night)|every\\s+weekdays?|on weekdays|weekdays|"
                                   L"every\\s+week|weekly|every\\s+(sun|mon|tue|wed|thu|fri|sat)[a-z]*?s?)\\b");
    int repeatDay = -1;
    if (spans.Search(every, m)) {
        const std::wstring s = m[0].str();
        if (m[1].matched) w.repeat = Repeat::Weekly, repeatDay = Weekday(m[1].str());
        else if (s.find(L"week") != std::wstring::npos && s.find(L"weekday") == std::wstring::npos) w.repeat = Repeat::Weekly;
        else if (s.find(L"weekday") != std::wstring::npos) w.repeat = Repeat::Weekdays;
        else w.repeat = Repeat::Daily;
    }
    int morningHour = -1;                                                         // an hour implied by the words ("every morning")
    if (w.repeat != Repeat::None) {
        const std::wstring s = m[0].str();
        if (s.find(L"morning") != std::wstring::npos) morningHour = 9;
        if (s.find(L"evening") != std::wstring::npos) morningHour = 19;
        if (s.find(L"night") != std::wstring::npos) morningHour = 21;
    }

    // ---- "in 20 minutes": exact, nothing else needed ---------------------------------------------------
    static const std::wregex relative(L"\\bin\\s+(\\d+|a|an|one|two|three|four|five|six|seven|eight|nine|ten|half an)\\s*"
                                      L"(minutes?|mins?|hours?|hrs?|days?|weeks?)\\b");
    if (spans.Search(relative, m)) {
        const std::wstring n = m[1].str();
        const int64_t amount = n == L"half an" ? 30 * kMinute : Number(n) * UnitSeconds(m[2].str());
        w.ok = true;
        w.at = now + amount;
    }

    // ---- the day -------------------------------------------------------------------------------------
    std::tm day = today;
    bool dayGiven = false, nightHint = morningHour >= 19;
    static const std::wregex months(L"\\b(\\d{1,2})(?:st|nd|rd|th)?\\s+(?:of\\s+)?(jan(?:uary)?|feb(?:ruary)?|mar(?:ch)?|apr(?:il)?|may|june?|july?|"
                                    L"aug(?:ust)?|sep(?:t(?:ember)?)?|oct(?:ober)?|nov(?:ember)?|dec(?:ember)?)\\b(?:,?\\s+(\\d{4}))?");
    static const std::wregex monthsFirst(L"\\b(jan(?:uary)?|feb(?:ruary)?|mar(?:ch)?|apr(?:il)?|may|june?|july?|aug(?:ust)?|sep(?:t(?:ember)?)?|"
                                         L"oct(?:ober)?|nov(?:ember)?|dec(?:ember)?)\\s+(\\d{1,2})(?:st|nd|rd|th)?\\b(?:,?\\s+(\\d{4}))?");
    static const std::wregex numeric(L"\\b(\\d{1,2})/(\\d{1,2})(?:/(\\d{2,4}))?\\b");   // day/month, as written in Bangladesh ("8.30" is a time)
    auto setDate = [&](int dayOfMonth, int month, int year) {
        if (dayOfMonth < 1 || dayOfMonth > 31 || month < 0 || month > 11) return;
        day.tm_mday = dayOfMonth;
        day.tm_mon = month;
        if (year > 0) day.tm_year = (year < 100 ? year + 2000 : year) - 1900;
        dayGiven = true;
        if (year <= 0 && At(day, 23, 59) < now) day.tm_year += 1;                // 4 March has passed: next year's
    };
    if (spans.Search(months, m)) setDate(std::stoi(m[1].str()), Month(m[2].str()), m[3].matched ? std::stoi(m[3].str()) : 0);
    else if (spans.Search(monthsFirst, m)) setDate(std::stoi(m[2].str()), Month(m[1].str()), m[3].matched ? std::stoi(m[3].str()) : 0);
    else if (spans.Search(numeric, m)) setDate(std::stoi(m[1].str()), std::stoi(m[2].str()) - 1, m[3].matched ? std::stoi(m[3].str()) : 0);

    static const std::wregex dayWord(L"\\b(day after tomorrow|tomorrow|tmrw|tonight|today|this (?:morning|afternoon|evening))\\b");
    if (!dayGiven && spans.Search(dayWord, m)) {
        const std::wstring s = m[1].str();
        if (s == L"day after tomorrow") day.tm_mday += 2;
        else if (s == L"tomorrow" || s == L"tmrw") day.tm_mday += 1;
        if (s == L"tonight" || s == L"this evening") nightHint = true;
        if (s == L"this morning") morningHour = 9;
        if (s == L"this afternoon") morningHour = 15;
        dayGiven = true;
    }
    static const std::wregex weekday(L"\\b(?:(next|this|on)\\s+)?(sunday|monday|tuesday|wednesday|thursday|friday|saturday)\\b");
    static const std::wregex weekdayShort(L"\\b(next|this|on)\\s+(sun|mon|tue|tues|wed|thu|thur|thurs|fri|sat)\\b");
    if (!dayGiven && (spans.Search(weekday, m) || spans.Search(weekdayShort, m))) {
        int ahead = (Weekday(m[2].str()) - today.tm_wday + 7) % 7;
        if (ahead == 0 || m[1].str() == L"next") ahead = ahead == 0 ? 7 : ahead;  // "friday" said on a Friday: next week's
        day.tm_mday += ahead;
        dayGiven = true;
    }

    // ---- the time --------------------------------------------------------------------------------------
    int hour = -1, minute = 0;
    bool meridiem = false;
    static const std::wregex clock(std::wstring(L"(?:\\bat\\s+|@\\s*)?\\b(\\d{1,2})[:.](\\d{2})\\s*(") + kTimeWord + L")?");
    static const std::wregex hourAmPm(std::wstring(L"(?:\\bat\\s+|@\\s*)?\\b(\\d{1,2})\\s*(") + kTimeWord + L")");
    static const std::wregex hourAt(L"(?:\\bat|@|\\b(?:alarm|wake me(?: up)?)\\s+(?:for|at))\\s*(\\d{1,2})\\b(?!\\s*(?:minutes?|mins?|hours?|days?|st|nd|rd|th))");
    static const std::wregex named(L"\\b(?:at\\s+)?(noon|midday|midnight)\\b");
    static const std::wregex partOfDay(L"\\b(?:in the\\s+)?(morning|afternoon|evening|night)\\b");
    auto applyMeridiem = [&](const std::wstring& ap) {
        if (ap.empty()) return;
        meridiem = true;
        if (ap[0] == L'p' && hour < 12) hour += 12;
        if (ap[0] == L'a' && hour == 12) hour = 0;
    };
    if (spans.Search(clock, m)) {
        hour = std::stoi(m[1].str()), minute = std::stoi(m[2].str());
        applyMeridiem(m[3].str());
        if (hour >= 13) meridiem = true;                                          // 20:30 is clear
    } else if (spans.Search(hourAmPm, m)) {
        hour = std::stoi(m[1].str());
        applyMeridiem(m[2].str());
    } else if (spans.Search(hourAt, m)) {
        hour = std::stoi(m[1].str());
        if (hour >= 13 || hour == 0) meridiem = true;
    } else if (spans.Search(named, m)) {
        hour = m[1].str() == L"midnight" ? 0 : 12, meridiem = true;
        if (hour == 0 && !dayGiven) day.tm_mday += 1;                              // "midnight" = the coming one
    } else if (dayGiven) {                                                        // "4 march 8": a bare hour right after the date
        static const std::wregex bare(L"^\\s*(?:,\\s*)?(\\d{1,2})\\b(?!\\s*(?:minutes?|mins?|hours?|days?|st|nd|rd|th|[/.:]))");
        const size_t afterDate = spans.cut.back().second;
        std::wsmatch b;
        const std::wstring rest = lower.substr(afterDate);
        if (std::regex_search(rest, b, bare)) {
            hour = std::stoi(b[1].str());
            if (hour >= 13 || hour == 0) meridiem = true;
            spans.cut.emplace_back(afterDate, afterDate + b.length(0));
        }
    }
    if (spans.Search(partOfDay, m)) {
        const std::wstring p = m[1].str();
        if (p == L"evening" || p == L"night") nightHint = true;
        if (hour < 0) hour = p == L"morning" ? 9 : p == L"afternoon" ? 15 : p == L"evening" ? 19 : 21, meridiem = true;
        else if (!meridiem && (p == L"morning")) meridiem = true;
        else if (!meridiem && hour < 12) hour += 12, meridiem = true;              // "8 in the evening"
    }
    if (hour < 0 && morningHour >= 0) hour = morningHour, meridiem = true;
    if (hour > 23 || minute > 59) hour = -1;                                      // "at 45": not a time

    // ---- put it together ----------------------------------------------------------------------------------
    if (!w.ok) {
        if (hour < 0 && !dayGiven && w.repeat == Repeat::None) {
            w.title = text;
            return w;                                                             // no time in it at all
        }
        w.ok = true;
        if (hour >= 0 && !meridiem && hour >= 1 && hour <= 12) {                  // "at 8": morning or evening?
            w.meridiemGuessed = true;
            if (nightHint) { if (hour < 12) hour += 12; }
            else if (hour <= 6 || hour == 12) { if (hour < 12) hour += 12; }      // 1-6 is usually the afternoon / evening
            if (!dayGiven && w.repeat == Repeat::None && At(day, hour, minute) <= now && hour < 12 && At(day, hour + 12, minute) > now)
                hour += 12;                                                       // 8 am has passed today, 8 pm has not
        }
        if (hour < 0) hour = 9;                                                   // a day without a time: in the morning
        if (w.repeat == Repeat::Weekly && repeatDay >= 0 && !dayGiven) day.tm_mday += (repeatDay - today.tm_wday + 7) % 7;
        w.at = At(day, hour, minute);
        if (w.at <= now) {
            if (!dayGiven || w.repeat != Repeat::None) w.at = NextOccurrence(w.at, w.repeat == Repeat::None ? Repeat::Daily : w.repeat, now);
        }
        if (w.repeat == Repeat::Weekdays) w.at = NextOccurrence(w.at, Repeat::Weekdays, now);   // a Saturday start moves to Monday
    }

    // ---- the title: everything that was not about time ------------------------------------------------------
    std::sort(spans.cut.begin(), spans.cut.end());
    std::wstring title;
    size_t pos = 0;
    for (auto& [a, b] : spans.cut) {
        if (a > pos) title += text.substr(pos, a - pos) + L" ";
        pos = std::max(pos, b);
    }
    if (pos < text.size()) title += text.substr(pos);
    static const std::wregex lead(L"^\\s*(?:(?:hey\\s+)?solu,?\\s+)?(?:please\\s+)?(?:remind me\\s+(?:to|about|of|that)?|set (?:a|an) (?:reminder|alarm)\\s*(?:for|to|about)?|"
                                  L"(?:a\\s+)?(?:reminder|alarm)\\s+(?:for|to|about))\\s*", std::regex::icase);
    static const std::wregex edges(L"^[\\s,.;:\\-–—]*(?:(?:on|at|for|by|the|this|next|from|starting|starts|is|,)\\s+)*|"
                                   L"(?:\\s+(?:on|at|for|by|the|this|next|from|starting|starts|and|is|in))*[\\s,.;:\\-–—]*$", std::regex::icase);
    title = std::regex_replace(title, lead, L"");
    title = std::regex_replace(title, std::wregex(L"\\s{2,}"), L" ");
    for (int i = 0; i < 3; ++i) title = std::regex_replace(title, edges, L"");
    static const std::wregex onlyVerb(L"^(?:set|make|create|add|start|put)(?:\\s+(?:an?|my|me))?$", std::regex::icase);
    if (std::regex_match(title, onlyVerb)) title.clear();                        // "set an [alarm for 7]": nothing left to name it
    if (title.empty()) title = Lower(text).find(L"alarm") != std::wstring::npos ? L"Alarm" : L"Reminder";
    title[0] = static_cast<wchar_t>(std::towupper(title[0]));
    w.title = title;
    return w;
}

int64_t FlipMeridiem(int64_t at) {
    std::tm tm = Local(at);
    tm.tm_hour = tm.tm_hour >= 12 ? tm.tm_hour - 12 : tm.tm_hour + 12;
    return FromLocal(tm);
}

int64_t NextOccurrence(int64_t at, Repeat repeat, int64_t after) {
    if (repeat == Repeat::None) return at;
    std::tm tm = Local(at);
    const int step = repeat == Repeat::Weekly ? 7 : 1;
    while (FromLocal(tm) <= after || (repeat == Repeat::Weekdays && (Local(FromLocal(tm)).tm_wday % 6 == 0))) {
        tm.tm_mday += step;                                                       // by calendar day: 7:00 stays 7:00 across summer time
        tm = Local(FromLocal(tm));
    }
    return FromLocal(tm);
}

std::wstring FormatEarly(int minutes) {
    wchar_t buf[48];
    if (minutes % (24 * 60) == 0) swprintf_s(buf, L"%d day%s before", minutes / 1440, minutes == 1440 ? L"" : L"s");
    else if (minutes % 60 == 0) swprintf_s(buf, L"%d h before", minutes / 60);
    else swprintf_s(buf, L"%d min before", minutes);
    return buf;
}

std::wstring FormatWhen(int64_t at, Repeat repeat, int64_t now) {
    static const wchar_t* const days[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    static const wchar_t* const months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    const std::tm t = Local(at), n = Local(now);
    wchar_t clock[16];
    swprintf_s(clock, L"%d:%02d %s", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12, t.tm_min, t.tm_hour < 12 ? L"AM" : L"PM");
    std::wstring dayText;
    if (repeat == Repeat::Daily) dayText = L"Daily";
    else if (repeat == Repeat::Weekdays) dayText = L"Weekdays";
    else if (repeat == Repeat::Weekly) dayText = std::wstring(L"Every ") + days[t.tm_wday];
    else {
        std::tm midnight = n;
        const int64_t start = At(midnight, 0, 0);
        const int64_t dayIndex = (at - start) / kDay - (at < start ? 1 : 0);
        wchar_t buf[48];
        if (dayIndex == 0) dayText = L"Today";
        else if (dayIndex == 1) dayText = L"Tomorrow";
        else if (t.tm_year != n.tm_year) swprintf_s(buf, L"%s, %s %d, %d", days[t.tm_wday], months[t.tm_mon], t.tm_mday, t.tm_year + 1900), dayText = buf;
        else swprintf_s(buf, L"%s, %s %d", days[t.tm_wday], months[t.tm_mon], t.tm_mday), dayText = buf;
    }
    return dayText + L" · " + clock;
}

std::wstring FormatCountdown(int64_t at, int64_t now) {
    const int64_t s = at - now;
    wchar_t buf[48];
    if (s <= 0) return L"now";
    if (s < kMinute) swprintf_s(buf, L"in %lld s", s);
    else if (s < kHour) swprintf_s(buf, L"in %lld min", (s + 59) / kMinute);
    else if (s < kDay) swprintf_s(buf, L"in %lld h %lld min", s / kHour, (s % kHour) / kMinute);
    else swprintf_s(buf, L"in %lld day%s", (s + kDay / 2) / kDay, (s + kDay / 2) / kDay == 1 ? L"" : L"s");
    return buf;
}

}  // namespace eflow
