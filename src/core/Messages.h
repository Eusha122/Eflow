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
constexpr UINT WM_APP_CAMERA = WM_APP + 12;      // camera service -> app: cameras, who uses them, or the privacy switch changed

} // namespace eflow
