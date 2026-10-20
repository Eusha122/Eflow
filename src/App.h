#pragma once
#include <windows.h>
#include <shellapi.h>

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "core/Gfx.h"
#include "island/CaptureOverlay.h"
#include "island/IslandWindow.h"
#include "services/ActivityMonitor.h"
#include "services/AgentMonitor.h"
#include "services/AlarmSound.h"
#include "services/ReminderStore.h"
#include "services/ShelfStore.h"
#include "mascot/MascotRenderer.h"
#include "services/AudioDevices.h"
#include "services/CameraService.h"
#include "services/DeviceWatcher.h"
#include "services/ScreenshotCatcher.h"
#include "services/HookInstaller.h"
#include "services/LyricsService.h"
#include "services/MediaService.h"
#include "services/NotificationMutes.h"
#include "services/NotificationService.h"
#include "services/PermissionServer.h"
#include "services/SoluService.h"
#include "services/SystemMonitor.h"

namespace eflow {

// Process-level shell: owns the GPU resources, one island per monitor, the tray icon, autostart and
// the fullscreen guard. Everything runs on the UI thread.
class App {
public:
    int Run();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);

    bool CreateMessageWindow();
    void LoadMascot();
    void BuildIslands();
    void SetMascotState(Mascot::State state);
    void OnMediaChanged();
    void OnPermissionsChanged();
    void OnNotificationsChanged();
    void OnSystemChanged();
    void OnAudioChanged();
    void OnCameraChanged();
    void OnSoluChanged();
    void OnSoluHotkey();
    // Live activities: downloads/copies (monitor), Solu's research and nudges, the focus timer, "done" flashes.
    void OnActivityChanged();
    void OnAgentsChanged();
    void OnShelfChanged();
    // Reminders: Tick rings what is due and sets the timer for the next one; the islands and Solu get the new list.
    void TickReminders();
    void OnRemindersChanged();
    void OnSoluReminder(const SoluAppEvent& e);  // "remind me ..." said to Solu
    void PushActivities();
    // Things plugged in: a short animated pill (phones, headphones), drives stay as a pill with Open and Eject.
    void OnDeviceEvent(const DeviceEvent& e);
    void Flash(const std::wstring& id, const std::wstring& glyph, const std::wstring& title, const std::wstring& detail, uint32_t accent);
    void OnSoluAppEvent(const SoluAppEvent& e);
    void StartFocus(int focusMinutes, int breakMinutes, int rounds);
    void StopFocus(bool finished);
    void TickFocus();
    void UpdateMood();
    void CheerAll();
    void TogglePrivacy();
    void StartCapture(const std::wstring& mode);
    void ToggleAiTool(AiTool tool);
    void RebuildGraphics();
    void UpdateFullscreenState();
    void AddTrayIcon();
    void ShowTrayMenu();

    static bool AutostartEnabled();
    static void SetAutostart(bool enable);
    static void ApplyFirstRunDefaults();
    void OnLyricsChanged();
    void SetLyricsOn(bool on);
    static std::wstring LyricsSettingPath();
    // Shoot mode (debug builds, AppShoot.cpp): scripted island content for filming the launch video.
    void OnShootCommand(const std::string& json);
    void ShootPushAll();
    void ShootPushPermissions();
    void ShootShelfAdd(const std::wstring& path);
    void ShootDismissNotification(uint32_t id);
    void ShootDecide(uint64_t id);
    void ShootMute(int side, bool muted);
    void ShootSelect(bool input, const std::wstring& id);

    Gfx gfx_;
    HWND msgWnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool appBar_ = false;
    NOTIFYICONDATAW tray_{};
    std::vector<HWINEVENTHOOK> hooks_;
    std::vector<std::unique_ptr<IslandWindow>> islands_;
    std::shared_ptr<const MascotAsset> mascotAsset_;   // null if the asset is missing or invalid
    MascotRenderer mascotRenderer_;
    bool mascotReady_ = false;
    std::unique_ptr<MediaService> media_;
    std::unique_ptr<LyricsService> lyrics_;
    bool lyricsOn_ = true;                         // live lyrics (the Music tab's lyrics button); kept in lyrics.json
    std::unique_ptr<PermissionServer> permissions_;
    std::unique_ptr<NotificationService> notifications_;
    std::unique_ptr<SystemMonitor> system_;
    std::unique_ptr<AudioDevices> audio_;
    std::unique_ptr<CameraService> camera_;
    std::unique_ptr<SoluService> solu_;
    uint32_t soluApprovalsVersion_ = 0;            // last list of Solu's approval requests the islands were given
    bool hotkey_ = false;
    std::unique_ptr<ActivityMonitor> activityMonitor_;
    std::unique_ptr<AgentMonitor> agents_;
    std::unique_ptr<ShelfStore> shelf_;
    ReminderStore reminders_;
    AlarmSound alarm_;
    std::vector<Alert> ringingWas_;                    // what was ringing at the last update (a new one starts the chime)
    std::string lastReminderContext_;
    AgentSnapshot agentSnap_;
    std::map<std::wstring, AgentStatus> agentWas_;     // per session: to notice when one finishes
    uint64_t hookCheck_ = 0;
    std::string lastAgentContext_;
    bool claudeHooked_ = false, codexHooked_ = false;
    CaptureOverlay capture_;
    RECT captured_{};
    std::wstring capturedMode_;
    std::vector<Activity> soluActivities_;          // research progress and nudges, from Solu
    std::vector<Activity> doneItems_;               // short "Downloaded ..." / "Connected" flashes
    std::unique_ptr<DeviceWatcher> devices_;
    std::unique_ptr<ScreenshotCatcher> shots_;      // a screenshot taken: a pill with the picture, and on the Shelf
    void OnScreenshot(const Screenshot& shot);
    void OnMark(const Activity& a);                 // a pill's mark was pressed: eject the drive, save the screenshot
    std::vector<Activity> drives_;                  // removable drives plugged in now (their pills stay)
    std::set<std::wstring> audioKnown_;              // audio endpoints already seen (a new one is announced)
    bool audioSeen_ = false;
    std::set<std::wstring> dismissed_;
    struct Focus {
        bool on = false, onBreak = false;
        int round = 1, rounds = 4, focusMin = 25, breakMin = 5;
        uint64_t endsAt = 0, length = 0;
    } focus_;
    bool privacy_ = false;
    bool showAnswer_ = false;                       // a capture was sent: open the island when Solu starts answering
    uint64_t cpuIdle_ = 0, cpuTotal_ = 0;
    int busyChecks_ = 0;
    int nudgeSeq_ = 0;
    uint64_t seenArrivals_ = 0;                        // arrivals already announced to the islands
    std::set<uint32_t> seenNotifications_;             // ids in the last snapshot: the rest are new
    NotificationMutes mutes_;                          // apps that may not open the island (Settings > Notifications)
    void PushMutes();
    bool shoot_ = false;                               // filming: the islands show only the director's content
    uint32_t shootSeq_ = 0;
    NotificationSnapshot shootNotifs_;
    std::vector<PermissionRequest> shootPerms_;
    AgentSnapshot shootAgents_;
    SoluSnapshot shootSolu_;
    std::vector<Activity> shootActs_;
    ShelfSnapshot shootShelf_;
    SystemSnapshot shootSystem_;
    AudioSnapshot shootAudio_;
    CameraSnapshot shootCamera_;
};

} // namespace eflow
