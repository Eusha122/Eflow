#include "services/ActivityMonitor.h"

#include <shlobj.h>

#include <cwctype>
#include <set>

#include "core/Log.h"

namespace eflow {
namespace {

std::wstring Size(uint64_t bytes) {
    wchar_t buf[32];
    if (bytes >= 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) swprintf_s(buf, L"%.0f MB", bytes / (1024.0 * 1024));
    else swprintf_s(buf, L"%.0f KB", bytes / 1024.0);
    return buf;
}

bool EndsWith(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::towlower(s[s.size() - n + i]) != tail[i]) return false;
    return true;
}

} // namespace

ActivityMonitor::ActivityMonitor(HWND notify, UINT message) : notify_(notify), message_(message) {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) downloads_ = path;
    CoTaskMemFree(path);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ActivityMonitor::~ActivityMonitor() {
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    if (stop_) CloseHandle(stop_);
}

void ActivityMonitor::Start() {
    thread_ = std::thread([this] { Loop(); });
}

std::vector<Activity> ActivityMonitor::Snapshot() {
    std::lock_guard lock(mutex_);
    return items_;
}

std::vector<std::wstring> ActivityMonitor::TakeFinished() {
    std::lock_guard lock(mutex_);
    return std::exchange(finished_, {});
}

void ActivityMonitor::Loop() {
    while (WaitForSingleObject(stop_, 1000) == WAIT_TIMEOUT) {
        std::vector<Activity> now;
        std::vector<std::wstring> finished;
        ScanDownloads(now, finished);
        ScanCopies(now, finished);
        bool changed;
        {
            std::lock_guard lock(mutex_);
            changed = !finished.empty() || now.size() != items_.size();
            for (size_t i = 0; !changed && i < now.size(); ++i)
                changed = now[i].id != items_[i].id || now[i].detail != items_[i].detail || now[i].progress != items_[i].progress;
            items_ = std::move(now);
            finished_.insert(finished_.end(), finished.begin(), finished.end());
        }
        if (changed) PostMessageW(notify_, message_, 0, 0);
    }
}

void ActivityMonitor::ScanDownloads(std::vector<Activity>& out, std::vector<std::wstring>& finished) {
    if (downloads_.empty()) return;
    std::set<std::wstring> present;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((downloads_ + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            const wchar_t* suffix = EndsWith(name, L".crdownload") ? L".crdownload" : EndsWith(name, L".part") ? L".part"
                                  : EndsWith(name, L".partial") ? L".partial" : nullptr;
            if (!suffix || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::wstring path = downloads_ + L"\\" + name;
            present.insert(path);
            const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            Seen& s = downloads[path];
            const uint64_t tick = GetTickCount64();
            if (!s.since) s.since = tick, s.firstSize = size;
            const uint64_t grew = size > s.size ? size - s.size : 0;
            s.size = size;
            s.name = name.substr(0, name.size() - wcslen(suffix));
            if (s.name.rfind(L"Unconfirmed ", 0) == 0) s.name = L"a file";        // Chrome names it once the save dialog is done
            Activity a;
            a.id = L"download:" + path;
            a.glyph = L"";                                                    // download arrow
            a.title = L"Downloading";
            a.detail = s.name + L"  ·  " + Size(size) + (grew ? L"  ·  " + Size(grew) + L"/s" : L"");
            a.accent = 0x0A84FF;
            out.push_back(std::move(a));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    for (auto it = downloads.begin(); it != downloads.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        // The temporary file is gone: finished if the real file is there (otherwise it was cancelled).
        const std::wstring final = downloads_ + L"\\" + it->second.name;
        if (GetFileAttributesW(final.c_str()) != INVALID_FILE_ATTRIBUTES) finished.push_back(it->second.name);
        it = downloads.erase(it);
    }
}

void ActivityMonitor::ScanCopies(std::vector<Activity>& out, std::vector<std::wstring>& finished) {
    struct Ctx {
        std::vector<std::pair<HWND, std::wstring>> found;
    } ctx;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        wchar_t cls[64];
        if (!IsWindowVisible(hwnd) || !GetClassNameW(hwnd, cls, 64) || wcscmp(cls, L"OperationStatusWindow") != 0) return TRUE;
        wchar_t title[256];
        GetWindowTextW(hwnd, title, 256);
        reinterpret_cast<Ctx*>(lp)->found.emplace_back(hwnd, title);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    std::map<HWND, std::wstring> now;
    for (const auto& [hwnd, title] : ctx.found) {
        now[hwnd] = title;
        Activity a;
        a.id = L"copy:" + std::to_wstring(reinterpret_cast<uintptr_t>(hwnd));
        a.glyph = L"";                                                        // copy
        a.title = L"Copying";
        int percent = -1;
        if (swscanf_s(title.c_str(), L"%d%%", &percent) == 1 && percent >= 0 && percent <= 100) a.progress = percent / 100.0f;
        a.detail = title.empty() ? L"Files" : title;
        a.accent = 0x30D158;
        out.push_back(std::move(a));
    }
    for (const auto& [hwnd, title] : copies)
        if (!now.count(hwnd)) finished.push_back(L"Copy");
    copies = std::move(now);
}

} // namespace eflow
