#pragma once
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/Image.h"

namespace eflow {

// What is playing, as published by apps through Windows' media sessions (Spotify, browsers incl.
// YouTube, the Media Player app, ...). A plain value: safe to copy across threads.
struct MediaInfo {
    bool hasSession = false;
    std::wstring title, artist, album;
    std::wstring sourceApp;                     // friendly name, e.g. "Spotify", "Chrome"
    bool playing = false;
    bool canToggle = false, canNext = false, canPrevious = false;
    bool canSeek = false;                       // the app accepts "jump to this position"
    bool canShuffle = false, shuffleOn = false;
    bool canRepeat = false;
    int repeat = 0;                             // 0 off, 1 repeat the track, 2 repeat the list
    double position = 0, duration = 0;          // seconds; duration 0 = unknown (live streams, some sites)
    ULONGLONG positionStamp = 0;                // GetTickCount64() when `position` was valid
    std::shared_ptr<const ImageBGRA> artwork;   // null if the app provides none
    uint64_t artworkId = 0;                     // changes whenever `artwork` does

    double PositionNow() const;                 // extrapolated while playing
};

enum class MediaCommand { TogglePlayPause, Next, Previous, Seek, Shuffle, Repeat };

struct MediaRequest {
    MediaCommand command;
    double seconds = 0;                         // Seek: the position to jump to; Shuffle: 1 on / 0 off; Repeat: the mode (0, 1, 2)
};

// Watches the system media sessions on a worker thread (event-driven, no polling) and posts
// WM_APP_MEDIA to `notify` whenever the snapshot changes; read it with Snapshot().
class MediaService {
public:
    explicit MediaService(HWND notify);
    ~MediaService();
    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;

    void Start();
    MediaInfo Snapshot() const;
    void Send(MediaCommand cmd, double seconds = 0);

private:
    void Run();
    struct Impl;                                // WinRT objects live only on the worker thread

    HWND notify_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool refresh_ = true;                       // something changed: rebuild the snapshot
    bool refreshArtwork_ = true;
    std::deque<MediaRequest> commands_;
    MediaInfo snapshot_;
};

} // namespace eflow
