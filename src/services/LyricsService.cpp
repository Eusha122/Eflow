#include "services/LyricsService.h"

#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Json.h"
#include "core/Messages.h"

namespace eflow {

namespace {

constexpr int kMissRetryDays = 7;               // a song LRCLIB did not have is asked about again after a week

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring Trim(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t-–—|:");
    if (a == std::wstring::npos) return {};
    const size_t b = s.find_last_not_of(L" \t-–—|:");
    return s.substr(a, b - a + 1);
}

std::string UrlEncode(const std::wstring& w) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : Utf8(w)) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::filesystem::path CacheDir() {
    PWSTR base = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = std::filesystem::path(base) / L"Eflow" / L"lyrics";
    CoTaskMemFree(base);
    return dir;
}

std::wstring CacheName(const std::wstring& key) {             // FNV-1a: short, stable, no personal text in file names
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : Utf8(key)) { h ^= c; h *= 1099511628211ull; }
    wchar_t name[32];
    swprintf_s(name, L"%016llx.lrc", static_cast<unsigned long long>(h));
    return name;
}

// "[01:02.34]text" lines (several stamps may share one line); metadata tags like [ar:...] are skipped.
std::vector<LyricLine> ParseLrc(const std::string& lrc) {
    std::vector<LyricLine> lines;
    std::istringstream in(lrc);
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::vector<double> stamps;
        size_t pos = 0;
        while (pos < raw.size() && raw[pos] == '[') {
            const size_t close = raw.find(']', pos);
            if (close == std::string::npos) break;
            int m = 0;
            double s = 0;
            if (sscanf_s(raw.substr(pos + 1, close - pos - 1).c_str(), "%d:%lf", &m, &s) == 2) stamps.push_back(m * 60.0 + s);
            pos = close + 1;
        }
        if (stamps.empty()) continue;
        std::wstring text = Trim(Wide(raw.substr(pos)));
        if (text == L"♪") text.clear();                        // LRCLIB marks breaks with a note
        for (double at : stamps) lines.push_back({at, text});
    }
    std::stable_sort(lines.begin(), lines.end(), [](const LyricLine& a, const LyricLine& b) { return a.at < b.at; });
    return lines;
}

// GET https://lrclib.net<path>. Returns the body; status gets the HTTP status (0 = no connection).
std::string HttpGet(const std::string& path, int& status) {
    status = 0;
    std::string body;
    HINTERNET session = WinHttpOpen(L"Eflow (Windows dynamic island; live lyrics)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, 4000, 4000, 6000, 6000);
    HINTERNET connect = WinHttpConnect(session, L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, 0);
    const std::wstring wpath = Wide(path);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                            WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
            body.append(chunk.data(), read);
            if (body.size() > 2 * 1024 * 1024) break;               // lyrics are small; never hold more
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

const std::string* Text(json::Value& v, const char* key) {
    const json::Value* f = v.Find(key);
    return f && f->type == json::Value::Type::String ? &f->text : nullptr;
}

double Number(json::Value& v, const char* key) {
    const json::Value* f = v.Find(key);
    return f && f->type == json::Value::Type::Number ? atof(f->text.c_str()) : 0.0;
}

} // namespace

int Lyrics::LineAt(double seconds) const {
    const auto it = std::upper_bound(lines.begin(), lines.end(), seconds, [](double s, const LyricLine& l) { return s < l.at; });
    return static_cast<int>(it - lines.begin()) - 1;
}

LyricsService::LyricsService(HWND notify) : notify_(notify) {
    worker_ = std::thread([this] { Run(); });
}

LyricsService::~LyricsService() {
    {
        std::lock_guard<std::mutex> g(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LyricsService::Clean(std::wstring& title, std::wstring& artist, const std::wstring& sourceApp) {
    // YouTube's auto channels are "Artist - Topic"; video titles carry "(Official Video)", "[Lyrics]", "| 4K"...
    if (artist.size() > 8 && Lower(artist).ends_with(L" - topic")) artist = artist.substr(0, artist.size() - 8);
    const bool browser = sourceApp.find(L"Chrome") != std::wstring::npos || sourceApp.find(L"Edge") != std::wstring::npos ||
                         sourceApp.find(L"Firefox") != std::wstring::npos || sourceApp.find(L"Brave") != std::wstring::npos ||
                         sourceApp.find(L"Opera") != std::wstring::npos;
    static const wchar_t* noise[] = {L"official", L"video", L"audio", L"lyric", L"visuali", L"hd", L"4k", L"mv", L"m/v", L"remaster",
                                     L"explicit", L"clean", L"full song", L"music video", L"color coded"};
    std::wstring t = title;
    for (const auto& [open, close] : {std::pair{L'(', L')'}, std::pair{L'[', L']'}}) {
        size_t a = 0;
        while ((a = t.find(open, a)) != std::wstring::npos) {
            const size_t b = t.find(close, a);
            if (b == std::wstring::npos) break;
            const std::wstring inside = Lower(t.substr(a + 1, b - a - 1));
            if (std::any_of(std::begin(noise), std::end(noise), [&](const wchar_t* n) { return inside.find(n) != std::wstring::npos; }))
                t.erase(a, b - a + 1);
            else
                a = b + 1;
        }
    }
    if (const size_t bar = t.find(L" | "); bar != std::wstring::npos) t = t.substr(0, bar);
    if (browser || artist.empty()) {                                // "Artist - Title" in a video title
        const size_t dash = t.find(L" - ");
        if (dash != std::wstring::npos && dash > 0) {
            if (artist.empty() || Lower(t.substr(0, dash)).find(Lower(artist)) != std::wstring::npos ||
                Lower(artist).find(Lower(t.substr(0, dash))) != std::wstring::npos) {
                artist = t.substr(0, dash);
                t = t.substr(dash + 3);
            }
        }
    }
    title = Trim(t);
    artist = Trim(artist);
}

void LyricsService::Request(const std::wstring& rawTitle, const std::wstring& rawArtist, const std::wstring& album, double duration,
                            const std::wstring& sourceApp) {
    std::wstring title = rawTitle, artist = rawArtist;
    Clean(title, artist, sourceApp);
    const std::wstring key = title.empty() ? L"" : Lower(artist) + L"\x1f" + Lower(title);
    {
        std::lock_guard<std::mutex> g(lock_);
        if (key == current_.key) return;                            // same song (a pause, a seek): nothing to do
        current_ = Lyrics{};
        current_.key = key;
        if (!key.empty()) {
            current_.status = Lyrics::Status::Loading;
            pending_ = std::make_unique<Job>(Job{key, title, artist, album, duration});
        }
    }
    wake_.notify_all();
    PostMessageW(notify_, WM_APP_LYRICS, 0, 0);
}

Lyrics LyricsService::Snapshot() const {
    std::lock_guard<std::mutex> g(lock_);
    return current_;
}

void LyricsService::Run() {
    const std::filesystem::path dir = CacheDir();
    std::error_code ec;
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    for (;;) {
        std::unique_ptr<Job> job;
        {
            std::unique_lock<std::mutex> g(lock_);
            wake_.wait(g, [this] { return stop_ || pending_; });
            if (stop_) return;
            job = std::move(pending_);
        }
        std::vector<LyricLine> lines;
        bool found = false, cached = false;
        const std::filesystem::path file = dir.empty() ? std::filesystem::path() : dir / CacheName(job->key);
        if (!file.empty()) {                                       // asked before: the answer is on disk
            std::ifstream in(file, std::ios::binary);
            if (in) {
                std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (data.rfind("#missing ", 0) == 0) {
                    const long long when = atoll(data.c_str() + 9);
                    cached = std::time(nullptr) - when < kMissRetryDays * 86400LL;
                } else {
                    lines = ParseLrc(data);
                    found = cached = !lines.empty();
                }
            }
        }
        if (!cached) {
            bool reachable = false;
            found = Fetch(*job, lines, reachable);
            if (!file.empty() && reachable) {                       // offline: ask again next time instead of remembering a miss
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                if (found) {
                    for (const LyricLine& l : lines) {
                        char stamp[24];
                        sprintf_s(stamp, "[%02d:%05.2f]", static_cast<int>(l.at) / 60, std::fmod(l.at, 60.0));
                        out << stamp << Utf8(l.text) << "\n";
                    }
                } else {
                    out << "#missing " << static_cast<long long>(std::time(nullptr)) << "\n";
                }
            }
        }
        {
            std::lock_guard<std::mutex> g(lock_);
            if (current_.key != job->key) continue;                 // the song changed while we were asking
            current_.status = found ? Lyrics::Status::Found : Lyrics::Status::Missing;
            current_.lines = std::move(lines);
        }
        PostMessageW(notify_, WM_APP_LYRICS, 0, 0);
    }
}

bool LyricsService::Fetch(const Job& job, std::vector<LyricLine>& out, bool& reachable) {
    reachable = false;
    auto synced = [&](json::Value& v) -> bool {
        const std::string* lrc = Text(v, "syncedLyrics");
        if (!lrc || lrc->empty()) return false;
        out = ParseLrc(*lrc);
        return !out.empty();
    };
    int status = 0;
    // 1. The exact song (title, artist, album, length within 2 s): the most reliable match.
    if (job.duration > 0) {
        std::string path = "/api/get?track_name=" + UrlEncode(job.title) + "&artist_name=" + UrlEncode(job.artist) +
                           "&duration=" + std::to_string(static_cast<int>(std::lround(job.duration)));
        if (!job.album.empty()) path += "&album_name=" + UrlEncode(job.album);
        std::string body = HttpGet(path, status);
        reachable = status != 0;
        if (status == 200) {
            json::Value v;
            std::string err;
            if (json::Parse(body, v, err) && synced(v)) return true;
        }
        if (!reachable) return false;
    }
    // 2. A search: the closest length among the results that have synced lyrics.
    std::wstring title = job.title;
    for (const wchar_t* feat : {L" (feat", L" (ft", L" feat.", L" ft."}) {
        const size_t f = Lower(title).find(feat);
        if (f != std::wstring::npos) title = title.substr(0, f);
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string path = attempt == 0 ? "/api/search?track_name=" + UrlEncode(title) + "&artist_name=" + UrlEncode(job.artist)
                                              : "/api/search?q=" + UrlEncode(job.artist + L" " + title);
        if (attempt == 1 && job.artist.empty()) break;
        std::string body = HttpGet(path, status);
        reachable = reachable || status != 0;
        if (status != 200) {
            if (status == 0) return false;
            continue;
        }
        json::Value list;
        std::string err;
        if (!json::Parse(body, list, err) || list.type != json::Value::Type::Array) continue;
        json::Value* best = nullptr;
        double bestDiff = 1e9;
        for (json::Value& item : list.items) {
            const std::string* lrc = Text(item, "syncedLyrics");
            if (!lrc || lrc->empty()) continue;
            const double diff = job.duration > 0 ? std::abs(Number(item, "duration") - job.duration) : 0;
            if (diff < bestDiff) { bestDiff = diff; best = &item; }
        }
        // a video can run longer than the song (an intro, an outro): allow some slack, but not a different song
        if (best && bestDiff <= (job.duration > 0 ? 20.0 : 1e9) && synced(*best)) return true;
    }
    return false;
}

} // namespace eflow
