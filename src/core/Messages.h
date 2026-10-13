#pragma once
#include <windows.h>

namespace eflow {

// Private window messages.
constexpr UINT WM_APP_FRAME = WM_APP + 1;        // island: render the next animation frame
constexpr UINT WM_APP_DEVICE_LOST = WM_APP + 2;  // island -> app: GPU device removed, rebuild
constexpr UINT WM_APP_TRAY = WM_APP + 3;         // tray icon callback
constexpr UINT WM_APP_APPBAR = WM_APP + 4;       // shell app-bar notifications (fullscreen app opened/closed)
constexpr UINT WM_APP_DEBUG_MASCOT = WM_APP + 5; // debug builds only: wParam = Mascot::State, for scripted tests

} // namespace eflow
