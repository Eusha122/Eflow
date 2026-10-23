#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace eflow {

struct DownloadResult {
    bool ok = false;
    int status = 0;             // HTTP status (0: never got a response)
    DWORD error = 0;            // WinHTTP / Win32 error when status is 0
    bool cancelled = false;
};

// Downloads `url` (https) to `destPath`, through a temporary file renamed into place on success, so a reader never
// sees a half-written file and a failed download leaves nothing behind. `onProgress(doneBytes, totalBytes)` is called
// from this thread as data arrives; totalBytes is 0 when the server does not report a length. Checking `*cancel`
// between reads lets the caller abort a long download promptly; pass nullptr for an uncancellable one.
DownloadResult DownloadFile(const std::wstring& url, const std::wstring& destPath, const std::atomic<bool>* cancel,
                           const std::function<void(uint64_t, uint64_t)>& onProgress);

} // namespace eflow
