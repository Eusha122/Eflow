// Unit tests for core/LoginCode (the verification-code finder). Run from eflow-tests (tests/when_test.cpp calls it).
#include <cstdio>
#include <string>

#include "core/LoginCode.h"

namespace {

int failed = 0;

void Expect(const wchar_t* text, const wchar_t* code) {
    const std::wstring got = eflow::FindLoginCode(text);
    if (got != code) {
        ++failed;
        std::wprintf(L"[FAIL] \"%s\"\n       got:  \"%s\"\n       want: \"%s\"\n", text, got.c_str(), code);
    } else {
        std::wprintf(L"[PASS] \"%s\" -> %s\n", text, *code ? code : L"no code");
    }
}

}  // namespace

int LoginCodeTests() {
    // what the big services send
    Expect(L"G-123456 is your Google verification code.", L"123456");
    Expect(L"Your WhatsApp code: 482-913\nDon't share this code with others", L"482913");
    Expect(L"123456 is your Amazon OTP. Do not share it with anyone.", L"123456");
    Expect(L"Your Instagram code is 123 456.", L"123456");
    Expect(L"Microsoft account\nSecurity code: 5821", L"5821");
    Expect(L"Use 48291 as your Facebook confirmation code", L"48291");
    Expect(L"Your Uber code: 4821. Reply STOP to 89203 to unsubscribe", L"4821");
    Expect(L"Steam Guard code: F4K2P", L"F4K2P");
    Expect(L"Your Discord verification code is 73918204", L"73918204");
    Expect(L"Telegram code: 51234", L"51234");
    // Bangla, and digits in Bangla
    Expect(L"আপনার ওটিপি ৫৮২৯১৩", L"582913");   // আপনার ওটিপি ৫৮২৯১৩
    Expect(L"bKash: আপনার কোড 7342. কাউকে বলবেন না", L"7342");
    // numbers around it that are not the code
    Expect(L"Use 7342 as your bKash verification code. Your balance is Tk 5,000", L"7342");
    Expect(L"Payment of $1234 received. Ref code 9981", L"9981");
    Expect(L"On 2026-10-20 your code is 5531", L"5531");
    Expect(L"Your PIN is 9086. Call 01712345678 if this wasn't you", L"9086");
    // no code at all
    Expect(L"Meeting at 10:30 tomorrow, room 4021", L"");
    Expect(L"Your order #123456 has shipped", L"");
    Expect(L"Call me at 01712345678", L"");
    Expect(L"Use promo code SAVE20 for 20% off", L"");
    Expect(L"Coupon code 2024 gets you 10% off", L"");
    Expect(L"I'll send you the code later", L"");
    Expect(L"Your verification code is 123456789", L"");          // nine digits: an account number, not a code
    Expect(L"Scanning the barcode 4829 failed", L"");             // "barcode" is not "code"
    Expect(L"spinning 1234 times", L"");                          // "spinning" is not "PIN"
    Expect(L"Battery at 85%, 4321 mAh", L"");
    Expect(L"", L"");
    // reading it
    if (eflow::SpacedCode(L"123456") != L"123 456" || eflow::SpacedCode(L"4821") != L"4821" || eflow::SpacedCode(L"F4K2P") != L"F4K2P")
        ++failed, std::wprintf(L"[FAIL] SpacedCode\n");
    return failed;
}
