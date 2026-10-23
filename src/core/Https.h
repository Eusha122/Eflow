#pragma once
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace eflow {

struct HttpRequest {
    std::wstring method = L"GET";
    std::wstring url;                                           // https://host/path?query
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    const uint8_t* body = nullptr;                              // not copied: must outlive Send
    size_t bodySize = 0;
    std::function<void(size_t)> onSent;                         // bytes of the body sent so far (uploads)
};

struct HttpResponse {
    int status = 0;                                             // 0: no answer (see error)
    DWORD error = 0;                                            // WinHTTP / Win32 error when status is 0
    std::string body;
    std::wstring etag;                                          // the ETag header, quotes removed
    int retryAfter = 0;                                         // seconds, from Retry-After (0: none)
    bool ok() const { return status >= 200 && status < 300; }
    bool cancelled() const { return error == ERROR_WINHTTP_OPERATION_CANCELLED; }
};

// Blocking HTTPS requests over WinHTTP (the system proxy and TLS settings apply). Thread-safe: several threads may Send at
// once; Cancel, from any thread, aborts what is in flight and fails every later Send until Reset. Bodies go out in pieces,
// so uploads report progress and can be cancelled midway.
class HttpClient {
public:
    explicit HttpClient(const wchar_t* userAgent);
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse Send(const HttpRequest& request);
    void Cancel();
    void Reset() { cancelled_ = false; }
    bool Cancelled() const { return cancelled_; }

private:
    HINTERNET session_ = nullptr;
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::set<HINTERNET> inFlight_;
};

} // namespace eflow
