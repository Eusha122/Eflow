#pragma once
#include <windows.h>

namespace eflow::mousehook {

// One low-level mouse hook for all islands, on its own thread (so a busy UI thread can never make the system mouse lag).
// While an island is closed it is click-through, so the hook is how it notices the user:
//   * a middle click or a wheel turn on the closed island
//                                              -> WM_APP_ISLAND_MIDDLE   (wParam 1 for the wheel; swallowed: the app below never sees it)
//   * the left / right button goes down or up  -> WM_APP_ISLAND_BUTTON   (wParam 1 down, 0 up: a held button may carry files)
//   * any left / right click, if asked for     -> WM_APP_CLICKAWAY       (the live conversation view closes on a click elsewhere)

void Register(HWND island);
void Unregister(HWND island);
// Where the closed island is on screen (empty: hidden or open), and whether clicks elsewhere should be reported.
void Update(HWND island, const RECT& closed, bool clickAway);

} // namespace eflow::mousehook
