#include "island/MouseHook.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/Messages.h"

namespace eflow::mousehook {
namespace {

struct Target {
    HWND hwnd = nullptr;
    RECT closed{};                 // screen pixels; empty when the island is open or hidden
    bool clickAway = false;
};

std::mutex g_mutex;
std::vector<Target> g_targets;
std::thread g_thread;
DWORD g_threadId = 0;
HHOOK g_hook = nullptr;
bool g_middleSwallowed = false;    // the matching button-up is swallowed too

LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const POINT pt = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp)->pt;
        std::lock_guard lock(g_mutex);
        if (wp == WM_MBUTTONDOWN || wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) {   // a middle click, or the wheel either way
            for (const Target& t : g_targets)
                if (PtInRect(&t.closed, pt)) {
                    PostMessageW(t.hwnd, WM_APP_ISLAND_MIDDLE, wp == WM_MBUTTONDOWN ? 0 : 1, MAKELPARAM(pt.x, pt.y));   // (screen point)
                    if (wp == WM_MBUTTONDOWN) g_middleSwallowed = true;
                    return 1;                                   // not passed on: Chrome must not close (or scroll) the tabs below
                }
        } else if (wp == WM_MBUTTONUP && g_middleSwallowed) {
            g_middleSwallowed = false;
            return 1;
        } else if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_LBUTTONUP || wp == WM_RBUTTONUP) {
            const bool down = wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN;
            for (const Target& t : g_targets) {
                PostMessageW(t.hwnd, WM_APP_ISLAND_BUTTON, down ? 1 : 0, 0);
                if (down && t.clickAway) PostMessageW(t.hwnd, WM_APP_CLICKAWAY, 0, 0);
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

void HookThread() {
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) Log("mouse hook: unavailable (%lu); middle-click will not open the island", GetLastError());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}             // a low-level hook needs a message loop on its thread
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
}

}  // namespace

void Register(HWND island) {
    std::lock_guard lock(g_mutex);
    g_targets.push_back({island, {}, false});
    if (!g_thread.joinable()) {
        g_thread = std::thread(HookThread);
        g_threadId = GetThreadId(g_thread.native_handle());
    }
}

void Unregister(HWND island) {
    std::thread stopping;
    {
        std::lock_guard lock(g_mutex);
        std::erase_if(g_targets, [&](const Target& t) { return t.hwnd == island; });
        if (!g_targets.empty() || !g_thread.joinable()) return;
        stopping = std::move(g_thread);
    }
    while (!PostThreadMessageW(g_threadId, WM_QUIT, 0, 0) && stopping.joinable())
        Sleep(1);                                                // (the thread may not have its queue yet)
    stopping.join();
}

void Update(HWND island, const RECT& closed, bool clickAway) {
    std::lock_guard lock(g_mutex);
    for (Target& t : g_targets)
        if (t.hwnd == island) t.closed = closed, t.clickAway = clickAway;
}

} // namespace eflow::mousehook
