// Unit tests for SafeShare's encryption (services/ShareCrypto) and the tab's email check. Run from eflow-tests.
// Known answers come from RFC 4648 (base64), FIPS 180-2 (SHA-256) and the GCM specification's test cases (AES-256).
#include <windows.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "services/PhoneSessions.h"
#include "services/SafeShareTypes.h"
#include "services/ShareActivity.h"
#include "services/ShareCrypto.h"

namespace sc = eflow::sharecrypto;

namespace {

int failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) ++failed;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
}

std::vector<uint8_t> Hex(const char* hex) {
    std::vector<uint8_t> out;
    for (const char* p = hex; p[0] && p[1]; p += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(p, 2), nullptr, 16)));
    return out;
}

std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

}  // namespace

// Command-line helpers for tools/safeshare_crosscheck.mjs, which checks Eflow against the website's own crypto code:
//   eflow-tests --share-seal <key> <in> <out>   encrypt a file the way Eflow uploads it
//   eflow-tests --share-open <key> <in> <out>   decrypt a file the website encrypted
//   eflow-tests --share-hash <password>         the room password's hash, printed
int ShareTool(int argc, wchar_t** argv) {
    const std::wstring verb = argv[1];
    if (verb == L"--share-hash" && argc == 3) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, utf8.data(), n, nullptr, nullptr);
        std::printf("%s\n", sc::Sha256Hex(utf8).c_str());
        return 0;
    }
    if ((verb != L"--share-seal" && verb != L"--share-open") || argc != 5) return 2;
    std::vector<uint8_t> key;
    const int n = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
    std::string keyText(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, keyText.data(), n, nullptr, nullptr);
    if (!sc::FromBase64Url(keyText, key)) return 3;
    sc::ChunkCipher cipher(key);
    FILE* in = nullptr;
    FILE* out = nullptr;
    if (!cipher.ok() || _wfopen_s(&in, argv[3], L"rb") != 0 || _wfopen_s(&out, argv[4], L"wb") != 0) {
        if (in) std::fclose(in);
        return 4;
    }
    const bool seal = verb == L"--share-seal";
    std::vector<uint8_t> chunk(seal ? sc::kChunkBytes : sc::kChunkBytes + sc::kSealedOverhead), result;
    int status = 0;
    for (bool first = true;; first = false) {
        const size_t got = std::fread(chunk.data(), 1, chunk.size(), in);
        if (got == 0 && !(seal && first)) break;                        // (an empty file still makes one chunk)
        if (!(seal ? cipher.Seal(chunk.data(), got, result) : cipher.Open(chunk.data(), got, result))) {
            status = 5;
            break;
        }
        std::fwrite(result.data(), 1, result.size(), out);
        if (got < chunk.size()) break;
    }
    std::fclose(in);
    std::fclose(out);
    return status;
}

int ShareTests() {
    // base64url (RFC 4648 section 10, without padding), and the two characters that differ from base64
    const char* const kPlain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* const kCoded[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    bool b64 = true;
    for (int i = 0; i < 7; ++i) {
        std::vector<uint8_t> back;
        b64 = b64 && sc::Base64Url(Bytes(kPlain[i])) == kCoded[i] && sc::FromBase64Url(kCoded[i], back) && back == Bytes(kPlain[i]);
    }
    Check(b64, "base64url: RFC 4648 vectors, both ways");
    Check(sc::Base64Url({0xFB, 0xFF}) == "-_8", "base64url: '-' and '_' instead of '+' and '/'");
    std::vector<uint8_t> junk;
    Check(!sc::FromBase64Url("ab!c", junk) && !sc::FromBase64Url("a", junk), "base64url: rejects bad input");
    const std::vector<uint8_t> key = sc::NewKey();
    std::vector<uint8_t> keyBack;
    Check(key.size() == 32 && sc::FromBase64Url(sc::Base64Url(key), keyBack) && keyBack == key && sc::Base64Url(key).size() == 43,
          "a room key: 32 random bytes, 43 characters in the link");

    // SHA-256 (the room password as the website sends it)
    Check(sc::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of \"abc\"");
    Check(sc::Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");

    // AES-256-GCM: GCM spec test cases 13 and 14 (zero key, zero IV) laid out as SafeShare chunks [IV | ciphertext | tag]
    const std::vector<uint8_t> zeroKey(32, 0);
    sc::ChunkCipher known(zeroKey);
    std::vector<uint8_t> plain;
    std::vector<uint8_t> case13 = Hex("000000000000000000000000530f8afbc74536b9a963b4f1c4cb738b");
    Check(known.ok() && known.Open(case13.data(), case13.size(), plain) && plain.empty(), "AES-256-GCM: test case 13 (empty)");
    std::vector<uint8_t> case14 = Hex("000000000000000000000000cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919");
    Check(known.Open(case14.data(), case14.size(), plain) && plain == std::vector<uint8_t>(16, 0), "AES-256-GCM: test case 14 (16 zero bytes)");

    // sealing: a fresh IV every time, opens again, and refuses tampering or the wrong key
    sc::ChunkCipher cipher(key);
    const std::vector<uint8_t> message = Bytes("SafeShare from Eflow: the quick brown fox jumps over the lazy dog");
    std::vector<uint8_t> a, b, opened;
    Check(cipher.Seal(message.data(), message.size(), a) && cipher.Seal(message.data(), message.size(), b) && a.size() == message.size() + 28 &&
              std::vector<uint8_t>(a.begin(), a.begin() + 12) != std::vector<uint8_t>(b.begin(), b.begin() + 12),
          "seal: IV | ciphertext | tag, a new IV each time");
    Check(cipher.Open(a.data(), a.size(), opened) && opened == message, "seal then open: the same bytes back");
    std::vector<uint8_t> tampered = a;
    tampered[20] ^= 1;
    Check(!cipher.Open(tampered.data(), tampered.size(), opened) && opened.empty(), "open: a changed byte is refused");
    sc::ChunkCipher other(sc::NewKey());
    Check(!other.Open(a.data(), a.size(), opened), "open: the wrong key is refused");
    std::vector<uint8_t> empty;
    Check(cipher.Seal(nullptr, 0, empty) && empty.size() == 28 && cipher.Open(empty.data(), empty.size(), opened) && opened.empty(),
          "an empty file is one 28-byte chunk");
    Check(!sc::ChunkCipher(std::vector<uint8_t>(16, 1)).ok(), "a key that is not 32 bytes is refused");

    // chunking (25 MB of plaintext per chunk)
    constexpr uint64_t mb25 = 25ull * 1024 * 1024;
    Check(sc::ChunkCount(0) == 1 && sc::ChunkCount(1) == 1 && sc::ChunkCount(mb25) == 1 && sc::ChunkCount(mb25 + 1) == 2 &&
              sc::ChunkCount(4ull * 1024 * 1024 * 1024) == 164,
          "chunk count");
    Check(sc::SealedSize(mb25 + 1) == mb25 + 1 + 2 * 28, "sealed size: 28 bytes more per chunk");

    // generated passwords: "xxxx-xxxx-xxxx", readable characters, never the same twice
    std::set<std::wstring> seen;
    bool shape = true;
    for (int i = 0; i < 200; ++i) {
        const std::wstring p = sc::MakePassword();
        shape = shape && p.size() == 14 && p[4] == L'-' && p[9] == L'-' &&
                p.find_first_of(L"0O1lI") == std::wstring::npos;
        seen.insert(p);
    }
    Check(shape && seen.size() == 200, "generated passwords: 12 readable characters, all different");

    // the recipient's address
    const wchar_t* const kGood[] = {L"rahim@gmail.com", L"a.b-c+tag@mail.example.co.uk", L"x@ab.co"};
    const wchar_t* const kBad[] = {L"", L"rahim", L"rahim@", L"@gmail.com", L"rahim@gmail", L"rahim@gmail.c", L"ra him@gmail.com",
                                   L"a@b@c.com", L"rahim@gmail..com", L"rahim@.com", L"<a@b.com>", L"a@b.com,c@d.com"};
    bool good = true, bad = true;
    for (const wchar_t* e : kGood) good = good && eflow::IsPlausibleEmail(e);
    for (const wchar_t* e : kBad) bad = bad && !eflow::IsPlausibleEmail(e);
    Check(good, "email: real addresses pass");
    Check(bad, "email: broken addresses are caught");

    // the access log, as api.safeshare.co returned it for the live test room (newest first), plus a later download
    const std::string log =
        R"({"logs":[{"id":"d2","event_type":"file_download","created_at":"2026-10-20T11:40:10.5+00:00","city":"Rajshahi","country":"BD",)"
        R"("browser":"Chrome","os":"Windows 10"},{"id":"x1","event_type":"leave","created_at":"2026-10-20T11:39:00+00:00"},)"
        R"({"id":"ef405a3b-3ffd-4185-8101-d0591cf57e42","event_type":"room_access","created_at":"2026-10-20T11:33:57.214152+00:00",)"
        R"("city":"Rajshahi","country":"BD","browser":"Chrome","os":"Windows 10"}]})";
    const int64_t openedAt = eflow::ParseIsoUtc("2026-10-20T11:33:57.214152+00:00");
    Check(openedAt == 1792496037, "ISO time with microseconds and +00:00");
    Check(eflow::ParseIsoUtc("2026-10-20T17:33:57+06:00") == openedAt && eflow::ParseIsoUtc("2026-10-20T11:33:57Z") == openedAt,
          "ISO time: other zones, and Z");
    Check(eflow::ParseIsoUtc("") == 0 && eflow::ParseIsoUtc("yesterday") == 0, "ISO time: unreadable is 0");
    Check(eflow::FormatIsoUtc(openedAt) == "2026-10-20T11:33:57.000Z", "ISO time written for the server");

    eflow::SentShare room;
    std::vector<std::string> counted;
    std::vector<eflow::ShareNews> news;
    const bool parsed = eflow::ApplyAccessLog(log, openedAt + 400, 600, room, counted, news);   // read 400 s after the open
    Check(parsed && room.opens == 1 && room.downloads == 1 && counted.size() == 3, "access log: one open, one download, 'leave' ignored");
    Check(news.size() == 2 && news[0].opened && !news[1].opened, "access log: both are news, oldest first");
    Check(room.lastSeen == L"Rajshahi, BD · Chrome on Windows 10", "access log: where it was opened");
    news.clear();
    Check(eflow::ApplyAccessLog(log, openedAt + 500, 600, room, counted, news) && news.empty() && room.opens == 1 && room.downloads == 1,
          "access log: read again, nothing is counted twice");
    eflow::SentShare later;
    std::vector<std::string> fresh;
    news.clear();
    eflow::ApplyAccessLog(log, openedAt + 3600, 600, later, fresh, news);
    Check(later.opens == 1 && later.downloads == 1 && news.empty(), "access log: an hour later it is counted, but not news");
    news.clear();
    // the phone link's visitors: the camera's preview opens the link first, then the browser (no cookie yet)
    {
        int next = 0;
        eflow::PhoneSessions phones([&] { return "s" + std::to_string(++next); });
        using Status = eflow::PhoneSessions::Status;
        const auto preview = phones.Arrive("", "192.168.0.7", true);
        Check(preview.knocked && preview.newCookie && phones.Asking(), "phone: the first visit asks to connect");
        const auto browser = phones.Arrive("", "192.168.0.7", false);
        Check(!browser.knocked && browser.session == preview.session && browser.newCookie,
              "phone: the browser after the camera's preview is the same visitor (same address)");
        Check(phones.StatusOf("", "192.168.0.7") == Status::Waiting, "phone: it waits for the answer");
        Check(phones.StatusOf("", "192.168.0.9") == Status::Busy, "phone: another device on the Wi-Fi is turned away");
        const auto second = phones.Arrive("", "192.168.0.9", true);
        Check(!second.knocked && phones.StatusOf(second.session, "192.168.0.9") == Status::Busy, "phone: ... and can't ask while one is asking");
        phones.Allow();
        Check(phones.IsAllowed(browser.session, "192.168.0.7") && phones.IsAllowed("", "192.168.0.7"),
              "phone: allowed, with or without its cookie");
        Check(!phones.IsAllowed(second.session, "192.168.0.9") && !phones.IsAllowed(preview.session + "x", "192.168.0.9"),
              "phone: the other device still can't get in");
        Check(phones.IsAllowed(browser.session, "192.168.0.8"), "phone: its cookie still works when its address changes");

        eflow::PhoneSessions denied([&] { return "d" + std::to_string(++next); });
        const auto knock = denied.Arrive("", "192.168.0.20", true);
        denied.Deny();
        Check(denied.StatusOf(knock.session, "192.168.0.20") == Status::Denied && denied.StatusOf("", "192.168.0.20") == Status::Denied,
              "phone: turned away, with or without its cookie");
        const auto again = denied.Arrive("", "192.168.0.20", true);
        Check(!again.knocked && denied.StatusOf(again.session, "192.168.0.20") == Status::Denied, "phone: a turned-away phone can't ask again");
        const auto someone = denied.Arrive("", "192.168.0.21", true);
        Check(someone.knocked, "phone: after a no, another device may ask");
    }

    Check(!eflow::ApplyAccessLog("<html>502</html>", openedAt, 600, later, fresh, news) &&
              !eflow::ApplyAccessLog(R"({"error":"Forbidden"})", openedAt, 600, later, fresh, news),
          "access log: an error page is not a log");
    return failed;
}
