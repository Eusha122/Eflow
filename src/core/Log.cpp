#include "core/Log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace eflow {
namespace {

std::filesystem::path LogPath() {
    static const std::filesystem::path path = [] {
        PWSTR base = nullptr;
        std::filesystem::path p;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
            p = std::filesystem::path(base) / L"Eflow";
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            p /= L"eflow.log";
        }
        CoTaskMemFree(base);
        return p;
    }();
    return path;
}

} // namespace

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds, msg);
    OutputDebugStringA(line);

    const auto path = LogPath();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f) {
        fputs(line, f);
        fclose(f);
    }
}

} // namespace eflow
