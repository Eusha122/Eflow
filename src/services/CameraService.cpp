#include "services/CameraService.h"

#include <initguid.h>

#include <psapi.h>
#include <setupapi.h>

#include <algorithm>
#include <cwctype>
#include <set>

#include "core/Log.h"
#include "services/DeviceReadiness.h"

namespace eflow {
namespace {

// KSCATEGORY_VIDEO_CAMERA: every camera (including virtual ones such as a phone used as a webcam).
const GUID kCameraCategory = {0xE5323777, 0xF976, 0x4f5b, {0x9B, 0x55, 0xB9, 0x46, 0x99, 0xC4, 0x6E, 0x44}};
const wchar_t kConsentKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
const wchar_t kConsentKeyDesktop[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam\\NonPackaged";

std::wstring ReadString(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buffer[64];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

std::vector<std::wstring> ListCameras() {
    std::vector<std::wstring> names;
    HDEVINFO set = SetupDiGetClassDevsW(&kCameraCategory, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return names;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &kCameraCategory, i, &iface)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        std::vector<char> buffer(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev{sizeof(dev)};
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, &dev)) continue;
        wchar_t name[256]{};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<PBYTE>(name), sizeof(name) - 2, nullptr) &&
            !SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<PBYTE>(name), sizeof(name) - 2, nullptr))
            continue;
        if (name[0] && std::find(names.begin(), names.end(), name) == names.end()) names.emplace_back(name);
    }
    SetupDiDestroyDeviceInfoList(set);
    return names;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Full paths of the programs running right now (lower case): used to ignore "in use" markers left behind by
// an app that crashed while it had the camera open.
std::set<std::wstring> RunningImages() {
    std::set<std::wstring> images;
    std::vector<DWORD> pids(2048);
    DWORD bytes = 0;
    if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &bytes)) return images;
    for (DWORD i = 0; i < bytes / sizeof(DWORD); ++i) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!process) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size)) images.insert(Lower(path));
        CloseHandle(process);
    }
    return images;
}

std::wstring Pretty(std::wstring name) {
    if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == L".exe") name.resize(name.size() - 4);
    if (!name.empty()) name[0] = static_cast<wchar_t>(std::towupper(name[0]));
    return name;
}

// Apps whose "last used" camera stamp has a start but no stop are using it right now.
std::vector<std::wstring> AppsUsingCamera() {
    std::vector<std::wstring> apps;
    std::set<std::wstring> running;
    bool haveRunning = false;
    for (int desktop = 0; desktop < 2; ++desktop) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, desktop ? kConsentKeyDesktop : kConsentKey, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        wchar_t name[512];
        for (DWORD i = 0;; ++i) {
            DWORD length = static_cast<DWORD>(std::size(name));
            if (RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (!desktop && std::wstring(name) == L"NonPackaged") continue;
            HKEY app;
            if (RegOpenKeyExW(key, name, 0, KEY_READ, &app) != ERROR_SUCCESS) continue;
            ULONGLONG start = 0, stop = 0;
            DWORD size = sizeof(start);
            RegGetValueW(app, nullptr, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &size);
            size = sizeof(stop);
            RegGetValueW(app, nullptr, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &size);
            RegCloseKey(app);
            if (start == 0 || stop != 0) continue;
            std::wstring label;
            if (desktop) {
                std::wstring path = name;
                std::replace(path.begin(), path.end(), L'#', L'\\');
                if (!haveRunning) running = RunningImages(), haveRunning = true;
                if (!running.count(Lower(path))) continue;         // stale marker: that program is not running any more
                const size_t slash = path.find_last_of(L'\\');
                label = Pretty(slash == std::wstring::npos ? path : path.substr(slash + 1));
            } else {
                std::wstring package = name;
                package = package.substr(0, package.find(L'_'));
                const size_t dot = package.find_last_of(L'.');
                label = dot == std::wstring::npos ? package : package.substr(dot + 1);
            }
            if (std::find(apps.begin(), apps.end(), label) == apps.end()) apps.push_back(label);
        }
        RegCloseKey(key);
    }
    return apps;
}

} // namespace

CameraService::CameraService(HWND notify, UINT message) : notify_(notify), message_(message) {}

CameraService::~CameraService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void CameraService::Start() { worker_ = std::thread([this] { Run(); }); }

void CameraService::Watch(int delta) {
    {
        std::lock_guard lock(mutex_);
        watchers_ = std::max(0, watchers_ + delta);
        if (delta > 0) refresh_ = true;
    }
    wake_.notify_all();
}

void CameraService::SetBlocked(bool blocked) {
    {
        std::lock_guard lock(mutex_);
        blockPending_ = blocked ? 1 : 0;
    }
    wake_.notify_all();
}

CameraSnapshot CameraService::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void CameraService::Run() {
    for (;;) {
        int block = -1;
        bool watching = false;
        {
            std::unique_lock lock(mutex_);
            // Nothing to do while the island is closed; while it is open, look again every second.
            wake_.wait_for(lock, watchers_ > 0 ? std::chrono::seconds(1) : std::chrono::minutes(5),
                           [&] { return stop_ || refresh_ || blockPending_ >= 0; });
            if (stop_) break;
            block = blockPending_;
            blockPending_ = -1;
            refresh_ = false;
            watching = watchers_ > 0;
        }
        if (block >= 0) {                                       // the same two switches Settings > Privacy > Camera flips
            const wchar_t* value = block ? L"Deny" : L"Allow";
            for (const wchar_t* key : {kConsentKey, kConsentKeyDesktop})
                RegSetKeyValueW(HKEY_CURRENT_USER, key, L"Value", REG_SZ, value, static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
            Log("camera: access %s", block ? "blocked" : "allowed");
        }
        (void)watching;

        CameraSnapshot fresh;
        fresh.valid = true;
        for (const std::wstring& name : ListCameras()) {
            const Readiness r = CheckReadiness(name);
            fresh.devices.push_back({name, r.ready, r.hint});
        }
        fresh.inUseBy = AppsUsingCamera();
        fresh.blocked = ReadString(HKEY_CURRENT_USER, kConsentKey, L"Value") == L"Deny" ||
                        ReadString(HKEY_CURRENT_USER, kConsentKeyDesktop, L"Value") == L"Deny";
        bool changed;
        {
            std::lock_guard lock(mutex_);
            changed = !snapshot_.valid || snapshot_.devices != fresh.devices || snapshot_.inUseBy != fresh.inUseBy ||
                      snapshot_.blocked != fresh.blocked;
            if (changed) snapshot_ = std::move(fresh);
        }
        if (changed) PostMessageW(notify_, message_, 0, 0);
    }
}

} // namespace eflow
