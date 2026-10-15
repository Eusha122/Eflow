#pragma once
#include <windows.h>

#include <functional>
#include <string>

namespace eflow {

// Quick capture: dims every screen; the user drags a rectangle (Esc or right-click cancels). The chosen area is cut
// out of the dimming so it is clear what Solu will look at. `done` receives the rectangle in physical screen pixels.
class CaptureOverlay {
public:
    CaptureOverlay() = default;
    ~CaptureOverlay();
    CaptureOverlay(const CaptureOverlay&) = delete;
    CaptureOverlay& operator=(const CaptureOverlay&) = delete;

    void Start(const std::wstring& mode, std::function<void(RECT, const std::wstring&)> done);
    bool Active() const { return hwnd_ != nullptr; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void Finish(bool ok);
    RECT Selection() const;

    HWND hwnd_ = nullptr;
    std::wstring mode_;
    std::function<void(RECT, const std::wstring&)> done_;
    POINT origin_{}, from_{}, to_{};
    bool dragging_ = false;
};

} // namespace eflow
