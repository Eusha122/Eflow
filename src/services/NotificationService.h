#pragma once
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Image.h"

namespace eflow {

struct NotificationItem {
    uint32_t id = 0;                            // Windows' notification id (stable while it is in the action center)
    std::wstring app;                           // display name of the sending app
    std::wstring appId;                         // its AppUserModelId (used to recognise and open chat apps)
    std::wstring title, body;
    ULONGLONG time = 0;                         // creation time, FILETIME ticks (100 ns since 1601, UTC)
    std::shared_ptr<const ImageBGRA> icon;      // the app's logo; null if unavailable
};

enum class NotificationAccess { Unknown, Allowed, Denied, Unsupported };

struct NotificationSnapshot {
    NotificationAccess access = NotificationAccess::Unknown;
    std::vector<NotificationItem> items;        // newest first
    uint64_t arrivals = 0;                      // counts notifications that appeared after startup
};

// Reads the action center through UserNotificationListener on a worker thread and posts WM_APP_NOTIFICATION
// to `notify` when the snapshot changes. Needs package identity (the sparse package, see
// tools\register_package.ps1) and the user's consent; without either it reports why and stays idle.
class NotificationService {
public:
    explicit NotificationService(HWND notify);
    ~NotificationService();
    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;

    // askNow false: Windows' consent prompt waits for AllowPrompt() (the welcome tour asks at its notifications step).
    void Start(bool askNow = true);
    void AllowPrompt();
    NotificationSnapshot Snapshot() const;
    void Dismiss(uint32_t id);
    void ClearAll();

private:
    void Run();
    struct Command { bool clearAll; uint32_t id; };

    HWND notify_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool refresh_ = true;
    bool mayAsk_ = true;
    std::deque<Command> commands_;
    NotificationSnapshot snapshot_;
};

} // namespace eflow
