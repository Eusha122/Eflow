#include "core/Https.h"

#include <algorithm>
#include <cwchar>

namespace eflow {
namespace {

constexpr int kResolveMs = 10000, kConnectMs = 15000, kSendMs = 60000, kReceiveMs = 60000;
constexpr size_t kPiece = 256 * 1024;                           // upload write size (progress and cancel granularity)

std::wstring Header(HINTERNET request, DWORD info) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

std::wstring CustomHeader(HINTERNET request, const wchar_t* name) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

struct Handle {                                                 // closes a WinHTTP handle on every way out
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

} // namespace

HttpClient::HttpClient(const wchar_t* userAgent) {
    session_ = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        WinHttpSetTimeouts(session_, kResolveMs, kConnectMs, kSendMs, kReceiveMs);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;           // (TLS 1.3 needs Windows 11 / Server 2022)
            WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
    }
}

HttpClient::~HttpClient() {
    Cancel();
    if (session_) WinHttpCloseHandle(session_);
}

void HttpClient::Cancel() {
    cancelled_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    for (HINTERNET h : inFlight_) WinHttpCloseHandle(h);          // the blocking call in Send returns with "cancelled"
    inFlight_.clear();
}

HttpResponse HttpClient::Send(const HttpRequest& r) {
    HttpResponse out;
    auto fail = [&](DWORD error) {
        out.status = 0;
        out.error = cancelled_ ? ERROR_WINHTTP_OPERATION_CANCELLED : error;
        return out;
    };
    if (!session_) return fail(ERROR_WINHTTP_NOT_INITIALIZED);
    if (cancelled_) return fail(ERROR_WINHTTP_OPERATION_CANCELLED);

    URL_COMPONENTS url{sizeof(url)};
    wchar_t host[256] = {}, path[4096] = {};
    url.lpszHostName = host;
    url.dwHostNameLength = ARRAYSIZE(host);
    url.lpszUrlPath = path;
    url.dwUrlPathLength = ARRAYSIZE(path);
    url.dwExtraInfoLength = 1;                                   // the query is appended to the path
    if (!WinHttpCrackUrl(r.url.c_str(), 0, 0, &url) || url.nScheme != INTERNET_SCHEME_HTTPS) return fail(ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    std::wstring object(url.lpszUrlPath, url.dwUrlPathLength);
    if (url.lpszExtraInfo) object.append(url.lpszExtraInfo, url.dwExtraInfoLength);

    Handle connect{WinHttpConnect(session_, host, url.nPort, 0)};
    if (!connect.h) return fail(GetLastError());
    HINTERNET request = WinHttpOpenRequest(connect.h, r.method.c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) return fail(GetLastError());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_) {
            WinHttpCloseHandle(request);
            return fail(ERROR_WINHTTP_OPERATION_CANCELLED);
        }
        inFlight_.insert(request);
    }
    auto release = [&] {                                         // the handle is ours to close unless Cancel took it
        std::lock_guard<std::mutex> lock(mutex_);
        if (inFlight_.erase(request)) WinHttpCloseHandle(request);
    };

    std::wstring headers;
    for (const auto& [name, value] : r.headers) headers += name + L": " + value + L"\r\n";
    const DWORD total = static_cast<DWORD>(r.bodySize);
    bool sent = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                   headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, total, 0) != FALSE;
    for (size_t at = 0; sent && at < r.bodySize;) {
        const DWORD piece = static_cast<DWORD>(std::min(kPiece, r.bodySize - at));
        DWORD written = 0;
        sent = WinHttpWriteData(request, r.body + at, piece, &written) != FALSE && written > 0;
        at += written;
        if (sent && r.onSent) r.onSent(at);
    }
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        const DWORD error = GetLastError();
        release();
        return fail(error);
    }

    const std::wstring status = Header(request, WINHTTP_QUERY_STATUS_CODE);
    out.status = static_cast<int>(wcstol(status.c_str(), nullptr, 10));
    out.etag = Header(request, WINHTTP_QUERY_ETAG);
    out.etag.erase(std::remove(out.etag.begin(), out.etag.end(), L'"'), out.etag.end());
    out.retryAfter = static_cast<int>(wcstol(Header(request, WINHTTP_QUERY_RETRY_AFTER).c_str(), nullptr, 10));
    if (out.retryAfter <= 0) out.retryAfter = static_cast<int>(wcstol(CustomHeader(request, L"RateLimit-Reset").c_str(), nullptr, 10));
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            const DWORD error = GetLastError();
            release();
            return fail(error);
        }
        if (available == 0) break;
        const size_t at = out.body.size();
        out.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, out.body.data() + at, available, &read)) {
            const DWORD error = GetLastError();
            release();
            return fail(error);
        }
        out.body.resize(at + read);
        if (out.body.size() > 16 * 1024 * 1024) break;            // (no API answer is this big)
    }
    release();
    return out;
}

} // namespace eflow
