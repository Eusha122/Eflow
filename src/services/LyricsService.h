#pragma once
#include <windows.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eflow {

// Time-synced lyrics for what is playing, from LRCLIB (lrclib.net: free and open, no account, no key).
// One small request per new song on a worker thread; every answer (a miss too) is kept on disk under
// %LOCALAPPDATA%\Eflow\lyrics, so a song played again needs no network. Posts WM_APP_LYRICS when the
// snapshot changes. Nothing runs while no song changes.
struct LyricLine {
    double at = 0;                              // seconds into the song
    std::wstring text;                          // empty: an instrumental break
};

struct Lyrics {
    enum class Status { None, Loading, Found, Missing } status = Status::None;
    std::wstring key;                           // which song these belong to (artist + title, normalised)
    std::vector<LyricLine> lines;               // sorted by time; only synced lyrics are kept

    int LineAt(double seconds) const;           // the line being sung at `seconds`, -1 before the first
};

class LyricsService {
public:
    explicit LyricsService(HWND notify);
    ~LyricsService();
    LyricsService(const LyricsService&) = delete;
    LyricsService& operator=(const LyricsService&) = delete;

    // The song changed (or not: same song = nothing happens). duration 0 = unknown.
    void Request(const std::wstring& title, const std::wstring& artist, const std::wstring& album, double duration,
                 const std::wstring& sourceApp);
    Lyrics Snapshot() const;

    // What a title/artist pair looks like once video noise ("(Official Video)", "Artist - Topic") is removed.
    static void Clean(std::wstring& title, std::wstring& artist, const std::wstring& sourceApp);

private:
    struct Job {
        std::wstring key, title, artist, album;
        double duration = 0;
    };
    void Run();
    bool Fetch(const Job& job, std::vector<LyricLine>& out, bool& reachable);

    HWND notify_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::unique_ptr<Job> pending_;
    Lyrics current_;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace eflow
