#include "core/Download.h"

#include <winhttp.h>

#include <algorithm>
#include <vector>

namespace eflow {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

uint64_t ContentLength(HINTERNET request) {
    wchar_t buf[32] = {};
    DWORD size = sizeof(buf);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size, WINHTTP_NO_HEADER_INDEX)) return 0;
    return _wcstoui64(buf, nullptr, 10);
}

} // namespace

DownloadResult DownloadFile(const std::wstring& url, const std::wstring& destPath, const std::atomic<bool>* cancel,
                           const std::function<void(uint64_t, uint64_t)>& onProgress) {
    DownloadResult out;
    auto fail = [&](DWORD error) {
        out.ok = false;
        out.error = error;
        return out;
    };
    if (cancel && cancel->load()) {
        out.cancelled = true;
        return out;
    }

    HINTERNET session = WinHttpOpen(L"Eflow/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return fail(GetLastError());
    Handle sessionGuard{session};
    WinHttpSetTimeouts(session, 10000, 15000, 30000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }

    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    parts.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return fail(ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    Handle connect{WinHttpConnect(session, host, parts.nPort, 0)};
    if (!connect.h) return fail(GetLastError());
    // WinHttpSendRequest follows redirects by default, which ollama.com's download link relies on.
    Handle request{WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE)};
    if (!request.h) return fail(GetLastError());
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return fail(GetLastError());

    wchar_t statusBuf[16] = {};
    DWORD statusSize = sizeof(statusBuf);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX, statusBuf, &statusSize, WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(wcstol(statusBuf, nullptr, 10));
    if (out.status < 200 || out.status >= 300) return fail(0);

    const uint64_t total = ContentLength(request.h);
    const std::wstring temp = destPath + L".download";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail(GetLastError());

    std::vector<char> buffer(256 * 1024);
    uint64_t done = 0;
    bool ok = true;
    for (;;) {
        if (cancel && cancel->load()) {
            out.cancelled = true;
            ok = false;
            break;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            out.error = GetLastError();
            ok = false;
            break;
        }
        if (available == 0) break;
        DWORD got = 0;
        const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!WinHttpReadData(request.h, buffer.data(), want, &got) || got == 0) {
            out.error = GetLastError();
            ok = false;
            break;
        }
        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), got, &written, nullptr) || written != got) {
            out.error = GetLastError();
            ok = false;
            break;
        }
        done += got;
        if (onProgress) onProgress(done, total);
    }
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return out;
    }
    if (!MoveFileExW(temp.c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        out.error = GetLastError();
        DeleteFileW(temp.c_str());
        return out;
    }
    out.ok = true;
    return out;
}

} // namespace eflow
