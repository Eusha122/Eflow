#pragma once
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Https.h"
#include "services/SafeShareTypes.h"

namespace eflow {

// Sends files with SafeShare (api.safeshare.co), exactly as the website does, so the recipient's experience is SafeShare's
// own: a password-protected room that closes after 10 hours, the files encrypted on this PC (services/ShareCrypto.h), and
// SafeShare's invitation email with the link. The room key travels only in that link; the room's owner token and key are
// kept for the 10 hours in %LOCALAPPDATA%\Eflow\safeshare.json, sealed for this Windows user (DPAPI), so the room can be
// watched ("opened", "downloaded") and closed early.
//
// It stays a good citizen of SafeShare's limits, which are per internet connection and shared with the website: uploads wait
// out a rate limit instead of failing, and watching the rooms uses about a tenth of the server's request budget.
//
// A worker thread does the network work; the app's window gets `changed` when Snapshot() has something new, and `event`
// (lParam: a heap Event, the receiver deletes it) when someone opens or downloads what was sent.
class SafeShareService {
public:
    struct Event {
        enum class Kind { Opened, Downloaded, CloseFailed } kind = Kind::Opened;
        std::wstring roomId, title, recipient, where;
    };

    SafeShareService(HWND notify, UINT changed, UINT event);
    ~SafeShareService();
    SafeShareService(const SafeShareService&) = delete;
    SafeShareService& operator=(const SafeShareService&) = delete;

    void Send(ShareSendRequest request);                 // ignored while a send is on its way
    void Cancel();                                       // stop it; a half-made room is closed again
    void Dismiss();                                      // the result was seen: back to idle
    void Close(const std::wstring& roomId);              // close a sent room for everyone, now
    std::wstring Link(const std::wstring& roomId) const; // the room's link, key included ("" if unknown)
    SafeShareSnapshot Snapshot() const;                  // (the phone part belongs to the phone link)

private:
    struct Room {
        SentShare info;
        std::string token, key;                          // owner token, and the key as it appears in the link
        bool complete = false;                           // false: still being sent (a crash left it half-made)
        std::vector<std::string> seen;                   // access-log entries already counted
    };
    struct Job {
        enum class Kind { Send, Close } kind = Kind::Send;
        ShareSendRequest request;
        std::wstring roomId;
        bool quiet = false;                              // Close: no message if it fails (cleanup)
    };
    struct FileToSend {
        std::wstring path, name;
        std::string contentType;
        uint64_t size = 0;
    };
    using Clock = std::chrono::steady_clock;

    void Run();
    void DoSend(const ShareSendRequest& request);
    void Upload(const FileToSend& file, const std::wstring& roomId, const std::string& token, const std::vector<uint8_t>& key);
    void DoClose(const std::wstring& roomId, bool quiet);
    void Watch();                                        // one access-log check, for the room whose turn it is
    HttpResponse Api(const wchar_t* method, const std::wstring& path, const std::string& body, const std::string& token,
                     bool waitOutLimits);
    bool Pause(std::chrono::milliseconds wait);          // false: cancelled or stopping
    void EditTransfer(const std::function<void(ShareTransfer&)>& edit);
    void Changed();
    void Load();
    void Save() const;                                   // (with mutex_ held)
    void Post(Event event) const;

    HWND notify_;
    UINT changedMessage_, eventMessage_;
    HttpClient http_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool stop_ = false;
    std::atomic<bool> cancel_{false};
    Clock::time_point nextWatch_ = Clock::time_point::max();
    size_t watchTurn_ = 0;
    std::vector<Room> rooms_;                            // newest first
    ShareTransfer transfer_;
    std::wstring sender_;
    int maxDevices_ = 1;
    uint32_t version_ = 0;
    mutable std::atomic<bool> posted_{false};
};

} // namespace eflow
