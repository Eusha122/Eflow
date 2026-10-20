#pragma once
#include <string>

namespace eflow {

// "G-123456 is your Google verification code" -> "123456". Finds the one-time login code in a notification's text: a 4 to 8
// digit number (or a short capital code like Steam's "F4K2P") close to a word such as code, OTP, PIN or verification, in
// English, Bangla and a few other languages. Amounts, phone numbers, dates, times and order numbers are not codes, and
// neither are promo or coupon codes. Returns the code without spaces or dashes, or empty. A pure function, unit tested
// in tests/login_code_test.cpp.
std::wstring FindLoginCode(const std::wstring& text);

// "123456" -> "123 456" (easier to read at a glance); other lengths are returned as they are.
std::wstring SpacedCode(const std::wstring& code);

} // namespace eflow
