#pragma once
#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Image.h"

namespace eflow {

struct Screenshot {
    std::wstring path;                           // the PNG on disk (Pictures\Screenshots, or Eflow's own captures folder)
    std::shared_ptr<const ImageBGRA> thumb;      // a small picture for the island
};

// Catches screenshots the moment they are taken so the island can offer them for one drag: images a screenshot tool puts
// on the clipboard (Win+Shift+S, PrtSc, Snipping Tool, ShareX, Greenshot, ...) and new files in Pictures\Screenshots
// (Win+PrtSc). Images copied by other apps are not screenshots and are left alone. Work happens on a worker thread; each
// screenshot is posted to `notify` as `message` with a heap-allocated Screenshot in lParam.
class ScreenshotCatcher {
public:
    ScreenshotCatcher(HWND notify, UINT message);
    ~ScreenshotCatcher();
    ScreenshotCatcher(const ScreenshotCatcher&) = delete;
    ScreenshotCatcher& operator=(const ScreenshotCatcher&) = delete;

    void OnClipboardUpdate();                    // call from WM_CLIPBOARDUPDATE (the app registers its window as a listener)
    // Keep a copy in Pictures\Screenshots (unless it is there already). Returns the saved path, or empty on failure.
    static std::wstring SaveToScreenshots(const std::wstring& path);

private:
    void Run();
    void TakeClipboard();
    void TakeFolder();
    void Announce(const std::wstring& path, const std::vector<uint8_t>& png, bool fromFolder);

    HWND notify_;
    UINT message_;
    HANDLE stop_ = nullptr, clipboard_ = nullptr;
    std::thread worker_;
    std::wstring folder_;                        // Pictures\Screenshots
    std::wstring lastPath_;
    std::deque<uint64_t> recent_;                // the pictures announced lately: the same one put on the clipboard again is old
    ULONGLONG lastAt_ = 0;
    DWORD lastSequence_ = 0;
};

} // namespace eflow
