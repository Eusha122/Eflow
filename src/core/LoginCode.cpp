#include "core/LoginCode.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <vector>

namespace eflow {
namespace {

constexpr int kMaxDistance = 60;                  // characters between the code and its word, at most

// Words that say "this is a login code". The ASCII ones must start a word (not "barcode"); `whole` ones must also end it.
struct Word { const wchar_t* text; bool whole; };
const Word kWords[] = {
    {L"code", false}, {L"otp", true}, {L"passcode", false}, {L"pin", true}, {L"verification", false}, {L"verify", false},
    {L"one-time", false}, {L"one time", false}, {L"2fa", true}, {L"two-factor", false}, {L"authentication", false},
    {L"código", false}, {L"codigo", false}, {L"codice", false}, {L"kode", true}, {L"kod", true},
    {L"কোড", false},               // কোড
    {L"ওটিপি", false},   // ওটিপি
    {L"পিন", false},               // পিন
    {L"код", false},               // код
    {L"验证码", false},               // 验证码
};
// A code for a discount is not a code to log in with.
const wchar_t* const kNotLogin[] = {L"promo", L"coupon", L"discount", L"voucher", L"referral", L"% off"};
// Money before a number: it is an amount.
const wchar_t* const kMoney[] = {L"$", L"€", L"£", L"₹", L"৳", L"tk", L"bdt", L"rs", L"usd", L"inr", L"eur"};

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool IsAsciiAlpha(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
bool IsAlnum(wchar_t c) { return IsDigit(c) || IsAsciiAlpha(c); }
bool IsSeparator(wchar_t c) { return c == L'-' || c == L'/' || c == L'.' || c == L':' || c == L','; }

// Digits of other scripts (Bangla, Arabic, Persian, Devanagari) as ASCII: codes arrive in all of them.
wchar_t AsciiDigit(wchar_t c) {
    for (wchar_t zero : {L'০', L'٠', L'۰', L'०'})
        if (c >= zero && c <= zero + 9) return static_cast<wchar_t>(L'0' + (c - zero));
    return c;
}

wchar_t Lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; }

struct Span { int start, end; };                  // [start, end)

struct Candidate {
    Span at;
    std::wstring code;
    bool letters = false;                         // a capital code like "F4K2P"
};

std::vector<Span> FindWords(const std::wstring& low) {
    std::vector<Span> found;
    for (const Word& w : kWords) {
        const std::wstring word = w.text;
        const bool ascii = std::all_of(word.begin(), word.end(), [](wchar_t c) { return c < 0x80; });
        for (size_t at = low.find(word); at != std::wstring::npos; at = low.find(word, at + 1)) {
            const size_t end = at + word.size();
            if (ascii && at > 0 && IsAsciiAlpha(low[at - 1])) continue;                   // "barcode"
            if (w.whole && end < low.size() && IsAsciiAlpha(low[end])) continue;         // "spinning", "pink"
            found.push_back({static_cast<int>(at), static_cast<int>(end)});
        }
    }
    return found;
}

// What stands just before `at` (skipping spaces), lower case: is it money?
bool MoneyBefore(const std::wstring& low, int at) {
    int i = at;
    while (i > 0 && low[i - 1] == L' ') --i;
    const std::wstring before = low.substr(0, i);
    for (const wchar_t* m : kMoney) {
        const std::wstring money = m;
        if (before.size() < money.size() || before.compare(before.size() - money.size(), money.size(), money) != 0) continue;
        const size_t edge = before.size() - money.size();
        if (IsAsciiAlpha(money[0]) && edge > 0 && IsAsciiAlpha(before[edge - 1])) continue;   // "hours" is not "rs"
        return true;
    }
    return false;
}

// Numbers that stand alone: "482913", "482 913", "482-913". Not part of a longer number, a word, an amount, a phone
// number, a date, a time, a decimal, an order number or a percentage.
std::vector<Candidate> FindNumbers(const std::wstring& s, const std::wstring& low) {
    std::vector<Candidate> out;
    const int n = static_cast<int>(s.size());
    for (int i = 0; i < n;) {
        if (!IsDigit(s[i]) || (i > 0 && IsAlnum(s[i - 1]))) {
            ++i;
            continue;
        }
        const int start = i;
        std::wstring digits;
        int j = i;
        while (j < n && IsDigit(s[j])) digits += s[j++];
        // Two groups of three or four digits with one space or dash between them read as one code.
        if (j + 1 < n && (s[j] == L' ' || s[j] == L'-') && IsDigit(s[j + 1]) && digits.size() >= 3) {
            int k = j + 1;
            std::wstring more;
            while (k < n && IsDigit(s[k])) more += s[k++];
            if (more.size() >= 3 && digits.size() + more.size() <= 8 && !(k < n && IsAlnum(s[k]))) digits += more, j = k;
        }
        i = j;
        if (digits.size() < 4 || digits.size() > 8) continue;
        if (j < n && IsAlnum(s[j])) continue;                                                 // "1234abc"
        if (start > 0 && (s[start - 1] == L'+' || s[start - 1] == L'#')) continue;           // a phone number, an order number
        if (start > 1 && IsSeparator(s[start - 1]) && IsDigit(s[start - 2])) continue;       // the end of a date, a time, an amount
        if (j + 1 < n && IsSeparator(s[j]) && IsDigit(s[j + 1])) continue;                   // the start of one
        if (j < n && s[j] == L'%') continue;
        if (MoneyBefore(low, start)) continue;
        out.push_back({{start, j}, digits, false});
    }
    return out;
}

// Short capital codes some services send ("F4K2P"): 5 to 8 capitals and digits, with at least one of each.
std::vector<Candidate> FindCapitalCodes(const std::wstring& s) {
    std::vector<Candidate> out;
    const int n = static_cast<int>(s.size());
    for (int i = 0; i < n;) {
        if (!IsAlnum(s[i])) {
            ++i;
            continue;
        }
        int j = i;
        while (j < n && IsAlnum(s[j])) ++j;
        const std::wstring token = s.substr(i, j - i);
        const bool capitals = std::all_of(token.begin(), token.end(), [](wchar_t c) { return IsDigit(c) || (c >= L'A' && c <= L'Z'); });
        const bool mixed = std::any_of(token.begin(), token.end(), IsDigit) && std::any_of(token.begin(), token.end(), IsAsciiAlpha);
        if (capitals && mixed && token.size() >= 5 && token.size() <= 8) out.push_back({{i, j}, token, true});
        i = j;
    }
    return out;
}

int Distance(Span a, Span b) { return a.end <= b.start ? b.start - a.end : b.end <= a.start ? a.start - b.end : 0; }

} // namespace

std::wstring FindLoginCode(const std::wstring& text) {
    std::wstring s = text, low;
    for (wchar_t& c : s) c = AsciiDigit(c);
    low.reserve(s.size());
    for (wchar_t c : s) low += Lower(c);
    for (const wchar_t* w : kNotLogin)
        if (low.find(w) != std::wstring::npos) return {};
    const std::vector<Span> words = FindWords(low);
    if (words.empty()) return {};

    std::vector<Candidate> all = FindNumbers(s, low);
    if (all.empty()) all = FindCapitalCodes(s);                    // digits first: a capital code only when there is no number
    std::wstring best;
    int bestScore = INT_MAX;
    for (const Candidate& c : all) {
        int near = INT_MAX;
        for (Span w : words) near = std::min(near, Distance(c.at, w));
        if (near > kMaxDistance) continue;
        int score = near;
        if (!c.letters && c.code.size() != 6) score += 4;          // six digits is the usual length
        if (!c.letters && c.code.size() == 4) {                    // a year is rarely a code
            const int year = std::wcstol(c.code.c_str(), nullptr, 10);
            if (year >= 1900 && year <= 2099) score += 30;
        }
        if (score < bestScore) bestScore = score, best = c.code;
    }
    return best;
}

std::wstring SpacedCode(const std::wstring& code) {
    const bool digits = std::all_of(code.begin(), code.end(), IsDigit);
    if (!digits || (code.size() != 6 && code.size() != 8)) return code;
    const size_t half = code.size() / 2;
    return code.substr(0, half) + L" " + code.substr(half);
}

} // namespace eflow
