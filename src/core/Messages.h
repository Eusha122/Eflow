#pragma once
#include <windows.h>

namespace eflow {

// Private window messages.
constexpr UINT WM_APP_FRAME = WM_APP + 1;        // island: render the next animation frame
constexpr UINT WM_APP_DEVICE_LOST = WM_APP + 2;  // island -> app: GPU device removed, rebuild
constexpr UINT WM_APP_TRAY = WM_APP + 3;         // tray icon callback
constexpr UINT WM_APP_APPBAR = WM_APP + 4;       // shell app-bar notifications (fullscreen app opened/closed)
constexpr UINT WM_APP_DEBUG_MASCOT = WM_APP + 5; // debug builds only: wParam = Mascot::State, for scripted tests
constexpr UINT WM_APP_MEDIA = WM_APP + 6;        // media service -> app: a new snapshot is available
constexpr UINT WM_APP_PERMISSION = WM_APP + 7;   // permission server -> app: the pending list changed
constexpr UINT WM_APP_NOTIFICATION = WM_APP + 8; // notification service -> app: a new snapshot is available
constexpr UINT WM_APP_SYSTEM = WM_APP + 9;       // system monitor -> app: a new sample is available
constexpr UINT WM_APP_DEBUG_TAB = WM_APP + 10;   // debug builds only: wParam = section to open and pin (-1 = release)
constexpr UINT WM_APP_AUDIO = WM_APP + 11;       // audio devices -> app: the device list or a level changed
constexpr UINT WM_APP_SOLU = WM_APP + 13;        // Solu helper -> app: its state, a new line or its voice level changed
constexpr UINT WM_APP_DEBUG_SOLU = WM_APP + 14;  // debug builds only: wParam 1 switch on, 0 off, 2 talk, 3 ask a test question
constexpr UINT WM_APP_CLICKAWAY = WM_APP + 15;   // island: the mouse was pressed somewhere (global hook, live view only)
constexpr UINT WM_APP_ACTIVITY = WM_APP + 16;    // activity monitor -> app: downloads / copies changed
constexpr UINT WM_APP_CAPTURED = WM_APP + 17;    // quick capture finished (the rectangle is kept by the app)
constexpr UINT WM_APP_AGENTS = WM_APP + 18;      // agent monitor -> app: Claude Code / Codex sessions changed
constexpr UINT WM_APP_LYRICS = WM_APP + 20;      // lyrics service -> app: lyrics for the playing song arrived (or are missing)
constexpr UINT WM_APP_SHELF = WM_APP + 19;       // shelf store -> app: the held files or an icon changed
constexpr UINT WM_APP_ISLAND_MIDDLE = WM_APP + 21;  // mouse hook -> island: middle click on the closed island (open it)
constexpr UINT WM_APP_ISLAND_BUTTON = WM_APP + 22;  // mouse hook -> island: left / right button down (wParam 1) or up (0)
constexpr UINT WM_APP_SCREENSHOT = WM_APP + 25;     // screenshot catcher -> app: a screenshot was taken (lParam: heap Screenshot)
constexpr UINT WM_APP_USB_DEVICE = WM_APP + 24;     // device watcher -> app: a USB device was identified (lParam: heap DeviceEvent)
constexpr UINT WM_APP_EJECTED = WM_APP + 23;        // device watcher -> app: an eject finished (lParam: heap EjectResult)
constexpr UINT WM_APP_CAMERA = WM_APP + 12;      // camera service -> app: cameras, who uses them, or the privacy switch changed

} // namespace eflow
