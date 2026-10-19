#pragma once
#include <string>
#include <utility>
#include <vector>

namespace eflow {

// Muted apps (Settings > Notifications). A muted app's notifications:
//   * no longer pop up on Windows: Eflow turns the app's banner off in Windows' own notification settings
//     (HKCU\...\Notifications\Settings\<app>\ShowBanner = 0), exactly what Settings > System > Notifications does;
//   * never open the island, make the cat react, count on the Notifications tab or reach Solu;
//   * still arrive in Windows' notification centre, so they stay in the island's Notifications list.
// Un-muting puts back whatever Windows had before. Kept in %LOCALAPPDATA%\Eflow\notification_mutes.json.
struct MutedApp {
    std::wstring id;       // the app's AppUserModelId (stable), or its name when Windows gives none
    std::wstring name;     // shown in the settings menu
};

struct NotificationMutes {
    bool all = false;                  // every app, including ones that start sending later
    std::vector<MutedApp> apps;
    uint32_t version = 0;

    bool IsMuted(const std::wstring& appId, const std::wstring& appName) const;
    void Set(const std::wstring& appId, const std::wstring& appName, bool muted);

    // Make Windows' banners match: off for every muted app among `known` (the apps that sent notifications), back to
    // how they were for the others. Returns true when anything changed.
    bool SyncBanners(const std::vector<MutedApp>& known);

    static NotificationMutes Load();
    void Save() const;

private:
    std::vector<std::pair<std::wstring, int>> banners_;   // apps whose banner Eflow turned off -> their old value (-1: not set)
};

} // namespace eflow
