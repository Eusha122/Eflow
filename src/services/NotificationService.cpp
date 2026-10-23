#include "services/NotificationService.h"

#include <appmodel.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "core/Log.h"
#include "core/Messages.h"
#include "core/PackageIdentity.h"

namespace eflow {

namespace wun = winrt::Windows::UI::Notifications;
namespace wunm = winrt::Windows::UI::Notifications::Management;
namespace wss = winrt::Windows::Storage::Streams;

namespace {

constexpr uint32_t kIconPx = 64;            // shown at ~28 DIP
constexpr size_t kMaxItems = 30;
constexpr auto kResync = std::chrono::seconds(20);   // safety net in case a change event is missed

ULONGLONG ToFileTime(winrt::Windows::Foundation::DateTime t) {
    return static_cast<ULONGLONG>(t.time_since_epoch().count());   // DateTime ticks are FILETIME ticks
}

std::shared_ptr<const ImageBGRA> LoadLogo(const wun::UserNotification& n) {
    try {
        const auto stream = n.AppInfo().DisplayInfo().GetLogo({static_cast<float>(kIconPx), static_cast<float>(kIconPx)});
        if (!stream) return nullptr;                  // classic desktop apps usually have none
        const auto source = stream.OpenReadAsync().get();
        const auto size = static_cast<uint32_t>(source.Size());
        if (size == 0 || size > 4u * 1024 * 1024) return nullptr;
        wss::Buffer buffer(size);
        source.ReadAsync(buffer, size, wss::InputStreamOptions::None).get();
        auto image = std::make_shared<ImageBGRA>();
        if (!DecodeImage(buffer.data(), buffer.Length(), 1.0f, kIconPx, *image)) return nullptr;
        return image;
    } catch (const winrt::hresult_error&) {
        return nullptr;
    }
}

} // namespace

NotificationService::NotificationService(HWND notify) : notify_(notify) {}

NotificationService::~NotificationService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void NotificationService::Start(bool askNow) {
    mayAsk_ = askNow;
    worker_ = std::thread([this] { Run(); });
}

void NotificationService::AllowPrompt() {
    {
        std::lock_guard lock(mutex_);
        mayAsk_ = true;
    }
    wake_.notify_all();
}

NotificationSnapshot NotificationService::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void NotificationService::Dismiss(uint32_t id) {
    {
        std::lock_guard lock(mutex_);
        commands_.push_back({false, id});
        snapshot_.items.erase(std::remove_if(snapshot_.items.begin(), snapshot_.items.end(),
                                             [id](const NotificationItem& i) { return i.id == id; }),
                              snapshot_.items.end());
    }
    wake_.notify_all();
}

void NotificationService::ClearAll() {
    {
        std::lock_guard lock(mutex_);
        commands_.push_back({true, 0});
        snapshot_.items.clear();
    }
    wake_.notify_all();
}

void NotificationService::Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto publishAccess = [this](NotificationAccess access) {
        {
            std::lock_guard lock(mutex_);
            snapshot_.access = access;
        }
        PostMessageW(notify_, WM_APP_NOTIFICATION, 0, 0);
    };

    if (!HasPackageIdentity()) {
        Log("notifications: no package identity (run tools\\register_package.ps1); feature disabled");
        publishAccess(NotificationAccess::Unsupported);
        winrt::uninit_apartment();
        return;
    }

    wunm::UserNotificationListener listener{nullptr};
    winrt::event_token changed{};
    try {
        listener = wunm::UserNotificationListener::Current();
        auto status = listener.GetAccessStatus();
        if (status == wunm::UserNotificationListenerAccessStatus::Unspecified) {
            bool wait;
            {
                std::lock_guard lock(mutex_);
                wait = !mayAsk_;
            }
            if (wait) {                                             // not asked yet: until the user gets to it
                publishAccess(NotificationAccess::Unknown);
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || mayAsk_; });
                if (stop_) {
                    lock.unlock();
                    winrt::uninit_apartment();
                    return;
                }
            }
            status = listener.RequestAccessAsync().get();          // shows Windows' consent prompt once
        }
        Log("notifications: access status %d", static_cast<int>(status));
        if (status != wunm::UserNotificationListenerAccessStatus::Allowed) {
            publishAccess(status == wunm::UserNotificationListenerAccessStatus::Denied ? NotificationAccess::Denied
                                                                                     : NotificationAccess::Unknown);
            winrt::uninit_apartment();
            return;
        }
        changed = listener.NotificationChanged([this](auto&&, auto&&) {
            {
                std::lock_guard lock(mutex_);
                refresh_ = true;
            }
            wake_.notify_all();
        });
    } catch (const winrt::hresult_error& e) {
        Log("notifications: listener unavailable (0x%08X)", static_cast<unsigned>(e.code()));
        publishAccess(NotificationAccess::Unsupported);
        winrt::uninit_apartment();
        return;
    }
    Log("notifications: listening");

    std::unordered_set<uint32_t> seen;            // ids already shown; anything new counts as an arrival
    std::unordered_map<uint32_t, std::shared_ptr<const ImageBGRA>> icons;   // per notification id; logos are cheap to refetch
    bool first = true;
    for (;;) {
        std::deque<Command> commands;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, kResync, [&] { return stop_ || refresh_ || !commands_.empty(); });
            if (stop_) break;
            refresh_ = false;
            commands.swap(commands_);
        }
        try {
            for (const Command& c : commands) {
                if (c.clearAll) listener.ClearNotifications();
                else listener.RemoveNotification(c.id);
            }

            const auto list = listener.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
            std::vector<NotificationItem> items;
            uint64_t arrivals = 0;
            std::unordered_set<uint32_t> now;
            for (const auto& n : list) {
                NotificationItem item;
                item.id = n.Id();
                now.insert(item.id);
                item.time = ToFileTime(n.CreationTime());
                try {
                    item.app = std::wstring(n.AppInfo().DisplayInfo().DisplayName());
                    item.appId = std::wstring(n.AppInfo().AppUserModelId());
                } catch (const winrt::hresult_error&) {}
                try {
                    const auto binding = n.Notification().Visual().GetBinding(wun::KnownNotificationBindings::ToastGeneric());
                    if (binding) {
                        std::wstring body;
                        uint32_t index = 0;
                        for (const auto& t : binding.GetTextElements()) {
                            if (index == 0) {
                                item.title = std::wstring(t.Text());
                            } else {
                                if (!body.empty()) body += L'\n';
                                body += std::wstring(t.Text());
                            }
                            ++index;
                        }
                        item.body = std::move(body);
                    }
                } catch (const winrt::hresult_error&) {}
                if (!first && !seen.count(item.id)) ++arrivals;
                if (auto it = icons.find(item.id); it != icons.end()) item.icon = it->second;
                else item.icon = icons[item.id] = LoadLogo(n);
                items.push_back(std::move(item));
            }
            std::sort(items.begin(), items.end(), [](const NotificationItem& a, const NotificationItem& b) { return a.time > b.time; });
            if (items.size() > kMaxItems) items.resize(kMaxItems);
            for (auto it = icons.begin(); it != icons.end();)           // forget logos of notifications that are gone
                it = now.count(it->first) ? std::next(it) : icons.erase(it);
            seen = std::move(now);
            first = false;

            bool differs = false;
            {
                std::lock_guard lock(mutex_);
                differs = snapshot_.access != NotificationAccess::Allowed || snapshot_.items.size() != items.size() || arrivals;
                for (size_t i = 0; !differs && i < items.size(); ++i)
                    differs = items[i].id != snapshot_.items[i].id || items[i].title != snapshot_.items[i].title ||
                              items[i].body != snapshot_.items[i].body;
                snapshot_.access = NotificationAccess::Allowed;
                snapshot_.items = std::move(items);
                snapshot_.arrivals += arrivals;
            }
            if (differs) PostMessageW(notify_, WM_APP_NOTIFICATION, 0, 0);
        } catch (const winrt::hresult_error& e) {
            Log("notifications: read failed 0x%08X; retrying", static_cast<unsigned>(e.code()));
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(5), [&] { return stop_; });
            refresh_ = true;
        }
    }

    listener.NotificationChanged(changed);
    listener = nullptr;
    winrt::uninit_apartment();
}

} // namespace eflow
