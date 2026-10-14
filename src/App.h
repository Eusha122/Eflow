#pragma once
#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <vector>

#include "core/Gfx.h"
#include "island/IslandWindow.h"
#include "mascot/MascotRenderer.h"
#include "services/AudioDevices.h"
#include "services/CameraService.h"
#include "services/HookInstaller.h"
#include "services/MediaService.h"
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
    void ToggleAiTool(AiTool tool);
    void RebuildGraphics();
    void UpdateFullscreenState();
    void AddTrayIcon();
    void ShowTrayMenu();

    static bool AutostartEnabled();
    static void SetAutostart(bool enable);
    static void ApplyFirstRunDefaults();

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
    std::unique_ptr<PermissionServer> permissions_;
    std::unique_ptr<NotificationService> notifications_;
    std::unique_ptr<SystemMonitor> system_;
    std::unique_ptr<AudioDevices> audio_;
    std::unique_ptr<CameraService> camera_;
    std::unique_ptr<SoluService> solu_;
    bool hotkey_ = false;
    uint64_t seenArrivals_ = 0;                        // arrivals already announced to the islands
};

} // namespace eflow
