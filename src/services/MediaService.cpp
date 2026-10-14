#include "services/MediaService.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cwctype>

#include "core/Log.h"
#include "core/Messages.h"

namespace eflow {

namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
using Session = wmc::GlobalSystemMediaTransportControlsSession;

namespace {

constexpr uint32_t kArtworkMaxSide = 192;   // shown at ~88 DIP; enough for 200% scaling

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// AppUserModelIds look like "Spotify.exe", "Chrome", "MSEdge", "308046B0AF4A39CB" (Firefox) or
// "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic". Map the common ones to friendly names.
std::wstring FriendlyAppName(const std::wstring& aumid) {
    const std::wstring id = Lower(aumid);
    struct Known { const wchar_t* key; const wchar_t* name; };
    static const Known known[] = {
        {L"spotify", L"Spotify"}, {L"chrome", L"Chrome"},           {L"msedge", L"Edge"},
        {L"firefox", L"Firefox"}, {L"308046b0af4a39cb", L"Firefox"}, {L"brave", L"Brave"},
        {L"opera", L"Opera"},     {L"vivaldi", L"Vivaldi"},          {L"zunemusic", L"Media Player"},
        {L"vlc", L"VLC"},         {L"applemusic", L"Apple Music"},   {L"tidal", L"TIDAL"},
        {L"deezer", L"Deezer"},   {L"amazonmusic", L"Amazon Music"}, {L"discord", L"Discord"},
    };
    for (const auto& k : known)
        if (id.find(k.key) != std::wstring::npos) return k.name;
    std::wstring name = aumid.substr(0, aumid.find(L'!'));          // packaged app: strip entry point
    if (const auto us = name.find(L'_'); us != std::wstring::npos) name.resize(us);
    if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == L".exe") name.resize(name.size() - 4);
    if (const auto dot = name.rfind(L'.'); dot != std::wstring::npos) name = name.substr(dot + 1);
    return name.empty() ? L"Media" : name;
}

double Seconds(winrt::Windows::Foundation::TimeSpan t) {
    return std::chrono::duration<double>(t).count();
}

} // namespace

double MediaInfo::PositionNow() const {
    if (!playing || !positionStamp) return position;
    const double p = position + (GetTickCount64() - positionStamp) / 1000.0;
    return duration > 0 ? std::min(p, duration) : p;
}

// ---- worker-thread state ---------------------------------------------------------------------

struct MediaService::Impl {
    wmc::GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    Session session{nullptr};
    winrt::event_token sessionsChanged{}, currentChanged{};
    winrt::event_token propsChanged{}, playbackChanged{}, timelineChanged{};
    uint64_t artworkCounter = 0;

    void Unhook() {
        if (!session) return;
        session.MediaPropertiesChanged(propsChanged);
        session.PlaybackInfoChanged(playbackChanged);
        session.TimelinePropertiesChanged(timelineChanged);
        session = nullptr;
    }

    // The system's "current" session, unless another one is actually playing.
    Session Pick() const {
        Session current = manager.GetCurrentSession();
        auto isPlaying = [](const Session& s) {
            if (!s) return false;
            const auto info = s.GetPlaybackInfo();          // null while a session is still starting up
            return info && info.PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        };
        if (isPlaying(current)) return current;
        for (const auto& s : manager.GetSessions())
            if (isPlaying(s)) return s;
        return current;
    }
};

MediaService::MediaService(HWND notify) : notify_(notify) {}

MediaService::~MediaService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void MediaService::Start() { worker_ = std::thread([this] { Run(); }); }

MediaInfo MediaService::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void MediaService::Send(MediaCommand cmd, double seconds) {
    {
        std::lock_guard lock(mutex_);
        commands_.push_back({cmd, seconds});
    }
    wake_.notify_all();
}

void MediaService::Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);   // blocking .get() is fine on MTA
    Impl impl;
    auto requestRefresh = [this](bool artwork) {
        {
            std::lock_guard lock(mutex_);
            refresh_ = true;
            refreshArtwork_ |= artwork;
        }
        wake_.notify_all();
    };

    try {
        impl.manager = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        impl.sessionsChanged = impl.manager.SessionsChanged([=](auto&&, auto&&) { requestRefresh(true); });
        impl.currentChanged = impl.manager.CurrentSessionChanged([=](auto&&, auto&&) { requestRefresh(true); });
    } catch (const winrt::hresult_error& e) {
        Log("media: session manager unavailable (0x%08X); music features disabled", static_cast<unsigned>(e.code()));
        winrt::uninit_apartment();
        return;
    }
    Log("media: watching system media sessions");

    for (;;) {
        bool doRefresh, doArtwork;
        std::deque<MediaRequest> commands;
        {
            std::unique_lock lock(mutex_);
            // Event-driven: wake on session events and commands. The slow timeout only resyncs
            // apps that occasionally forget to signal a change; progress is extrapolated, not polled.
            wake_.wait_for(lock, std::chrono::seconds(30), [&] { return stop_ || refresh_ || !commands_.empty(); });
            if (stop_) break;
            doRefresh = refresh_;
            doArtwork = refreshArtwork_;
            refresh_ = refreshArtwork_ = false;
            commands.swap(commands_);
        }

        try {
            for (const MediaRequest& req : commands) {
                if (!impl.session) break;
                switch (req.command) {
                case MediaCommand::TogglePlayPause: impl.session.TryTogglePlayPauseAsync().get(); break;
                case MediaCommand::Next: impl.session.TrySkipNextAsync().get(); break;
                case MediaCommand::Previous: impl.session.TrySkipPreviousAsync().get(); break;
                case MediaCommand::Seek:                               // 100 ns ticks from the start of the media
                    impl.session.TryChangePlaybackPositionAsync(static_cast<int64_t>(std::max(0.0, req.seconds) * 1e7)).get();
                    break;
                case MediaCommand::Shuffle: impl.session.TryChangeShuffleActiveAsync(req.seconds > 0.5).get(); break;
                case MediaCommand::Repeat:
                    impl.session.TryChangeAutoRepeatModeAsync(req.seconds > 1.5 ? winrt::Windows::Media::MediaPlaybackAutoRepeatMode::List
                                                              : req.seconds > 0.5 ? winrt::Windows::Media::MediaPlaybackAutoRepeatMode::Track
                                                                                  : winrt::Windows::Media::MediaPlaybackAutoRepeatMode::None).get();
                    break;
                }
                doRefresh = true;
            }

            Session pick = impl.Pick();
            if (pick != impl.session) {                       // follow the active session's events
                impl.Unhook();
                impl.session = pick;
                if (pick) {
                    impl.propsChanged = pick.MediaPropertiesChanged([=](auto&&, auto&&) { requestRefresh(true); });
                    impl.playbackChanged = pick.PlaybackInfoChanged([=](auto&&, auto&&) { requestRefresh(false); });
                    impl.timelineChanged = pick.TimelinePropertiesChanged([=](auto&&, auto&&) { requestRefresh(false); });
                }
                doRefresh = doArtwork = true;
            }

            MediaInfo info;
            {
                std::lock_guard lock(mutex_);
                info = snapshot_;                              // keeps the artwork unless it changes
            }
            const MediaInfo before = info;
            if (!impl.session) {
                info = MediaInfo{};
            } else {
                const auto playback = impl.session.GetPlaybackInfo();
                const auto timeline = impl.session.GetTimelineProperties();
                if (!playback || !timeline || !playback.Controls()) {   // a brand-new session has no data yet
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    std::lock_guard lock(mutex_);
                    refresh_ = true;                           // try again shortly
                    continue;
                }
                const auto controls = playback.Controls();
                info.hasSession = true;
                info.playing = playback.PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                info.canToggle = controls.IsPlayPauseToggleEnabled() || controls.IsPlayEnabled() || controls.IsPauseEnabled();
                info.canNext = controls.IsNextEnabled();
                info.canPrevious = controls.IsPreviousEnabled();
                info.canSeek = controls.IsPlaybackPositionEnabled();
                info.canShuffle = controls.IsShuffleEnabled();
                info.canRepeat = controls.IsRepeatEnabled();
                info.shuffleOn = false;
                if (const auto shuffle = playback.IsShuffleActive()) info.shuffleOn = shuffle.Value();
                info.repeat = 0;
                if (const auto mode = playback.AutoRepeatMode()) {
                    using Mode = winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
                    info.repeat = mode.Value() == Mode::Track ? 1 : mode.Value() == Mode::List ? 2 : 0;
                }
                info.sourceApp = FriendlyAppName(std::wstring(impl.session.SourceAppUserModelId()));

                info.duration = std::max(0.0, Seconds(timeline.EndTime() - timeline.StartTime()));
                double position = Seconds(timeline.Position() - timeline.StartTime());
                // Apps update the timeline sparsely; extrapolate from when they last did.
                const auto updated = timeline.LastUpdatedTime();
                const auto age = winrt::clock::now() - updated;
                const double ageSec = std::chrono::duration<double>(age).count();
                if (info.playing && ageSec > 0 && ageSec < 3600) position += ageSec;
                info.position = std::max(0.0, info.duration > 0 ? std::min(position, info.duration) : position);
                info.positionStamp = GetTickCount64();

                if (doRefresh || doArtwork) {
                    const auto props = impl.session.TryGetMediaPropertiesAsync().get();
                    const std::wstring title(props.Title()), artist(props.Artist());
                    const bool trackChanged = title != info.title || artist != info.artist;
                    info.title = title;
                    info.artist = artist;
                    info.album = std::wstring(props.AlbumTitle());
                    if (doArtwork || trackChanged) {
                        info.artwork.reset();
                        if (const auto thumb = props.Thumbnail()) {
                            const auto stream = thumb.OpenReadAsync().get();
                            const auto size = static_cast<uint32_t>(stream.Size());
                            if (size > 0 && size < 16u * 1024 * 1024) {
                                wss::Buffer buffer(size);
                                stream.ReadAsync(buffer, size, wss::InputStreamOptions::None).get();
                                auto image = std::make_shared<ImageBGRA>();
                                if (DecodeImage(buffer.data(), buffer.Length(), 1.0f, kArtworkMaxSide, *image))
                                    info.artwork = std::move(image);
                            }
                        }
                        info.artworkId = ++impl.artworkCounter;
                    }
                }
            }

            const bool changed = info.hasSession != before.hasSession || info.playing != before.playing ||
                                 info.title != before.title || info.artist != before.artist ||
                                 info.sourceApp != before.sourceApp || info.artworkId != before.artworkId ||
                                 info.canToggle != before.canToggle || info.canNext != before.canNext ||
                                 info.canPrevious != before.canPrevious || info.canSeek != before.canSeek ||
                                 info.canShuffle != before.canShuffle || info.shuffleOn != before.shuffleOn ||
                                 info.canRepeat != before.canRepeat || info.repeat != before.repeat ||
                                 std::abs(info.duration - before.duration) > 0.5 ||
                                 std::abs(info.PositionNow() - before.PositionNow()) > 1.5;
            {
                std::lock_guard lock(mutex_);
                snapshot_ = info;
            }
            if (changed) PostMessageW(notify_, WM_APP_MEDIA, 0, 0);
        } catch (const winrt::hresult_error& e) {
            // Apps come and go; a session can vanish mid-call. Drop it and pick again next time.
            Log("media: session error 0x%08X; resyncing", static_cast<unsigned>(e.code()));
            impl.Unhook();
            std::lock_guard lock(mutex_);
            refresh_ = refreshArtwork_ = true;
        }
    }

    impl.Unhook();
    if (impl.manager) {
        impl.manager.SessionsChanged(impl.sessionsChanged);
        impl.manager.CurrentSessionChanged(impl.currentChanged);
    }
    impl.manager = nullptr;
    winrt::uninit_apartment();
}

} // namespace eflow
