#include "services/NotificationMutes.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/Json.h"
#include "core/Utf8.h"

namespace eflow {
namespace {

const wchar_t kSettingsKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\";

std::wstring StorePath() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    dir += L"\\Eflow";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\notification_mutes.json";
}

std::wstring Key(const std::wstring& appId, const std::wstring& appName) { return appId.empty() ? appName : appId; }

// Windows' per-app "Show notification banners" (what Settings > System > Notifications > <app> changes).
int ReadBanner(const std::wstring& appId) {
    DWORD value = 0, size = sizeof(value);
    const LSTATUS s = RegGetValueW(HKEY_CURRENT_USER, (kSettingsKey + appId).c_str(), L"ShowBanner", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return s == ERROR_SUCCESS ? static_cast<int>(value) : -1;
}

void WriteBanner(const std::wstring& appId, int value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (kSettingsKey + appId).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (value < 0) {
        RegDeleteValueW(key, L"ShowBanner");                    // it was never set: Windows' default again
    } else {
        const DWORD v = static_cast<DWORD>(value);
        RegSetValueExW(key, L"ShowBanner", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    }
    RegCloseKey(key);
}

bool IsAppId(const std::wstring& id) {                          // Windows keeps settings for every app that has sent a notification
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (kSettingsKey + id).c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

}  // namespace

bool NotificationMutes::IsMuted(const std::wstring& appId, const std::wstring& appName) const {
    if (all) return true;
    const std::wstring key = Key(appId, appName);
    return std::any_of(apps.begin(), apps.end(), [&](const MutedApp& a) { return a.id == key; });
}

void NotificationMutes::Set(const std::wstring& appId, const std::wstring& appName, bool muted) {
    const std::wstring key = Key(appId, appName);
    std::erase_if(apps, [&](const MutedApp& a) { return a.id == key; });
    if (muted) apps.push_back({key, appName});
    ++version;
}

bool NotificationMutes::SyncBanners(const std::vector<MutedApp>& known) {
    std::vector<std::wstring> wanted;                            // apps whose banner should be off
    for (const MutedApp& a : known)
        if (IsAppId(a.id) && IsMuted(a.id, a.name)) wanted.push_back(a.id);
    for (const MutedApp& a : apps)
        if (IsAppId(a.id) && std::find(wanted.begin(), wanted.end(), a.id) == wanted.end()) wanted.push_back(a.id);
    bool changed = false;
    for (const std::wstring& id : wanted) {
        if (std::any_of(banners_.begin(), banners_.end(), [&](const auto& b) { return b.first == id; })) continue;
        banners_.emplace_back(id, ReadBanner(id));               // remember how it was, then switch it off
        WriteBanner(id, 0);
        changed = true;
    }
    for (auto it = banners_.begin(); it != banners_.end();) {
        if (std::find(wanted.begin(), wanted.end(), it->first) == wanted.end()) {
            WriteBanner(it->first, it->second);                  // un-muted: exactly as it was before
            it = banners_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

NotificationMutes NotificationMutes::Load() {
    NotificationMutes m;
    std::ifstream file(StorePath(), std::ios::binary);
    if (!file) return m;
    std::stringstream text;
    text << file.rdbuf();
    json::Value root;
    std::string error;
    if (!json::Parse(text.str(), root, error) || root.type != json::Value::Type::Object) return m;
    if (json::Value* all = root.Find("all"); all && all->type == json::Value::Type::Bool) m.all = all->boolean;
    if (json::Value* list = root.Find("apps"); list && list->type == json::Value::Type::Array)
        for (json::Value& v : list->items) {
            json::Value* id = v.Find("id");
            json::Value* name = v.Find("name");
            if (id && !id->text.empty()) m.apps.push_back({FromUtf8(id->text), name ? FromUtf8(name->text) : FromUtf8(id->text)});
        }
    if (json::Value* list = root.Find("banners_off"); list && list->type == json::Value::Type::Array)
        for (json::Value& v : list->items) {
            json::Value* id = v.Find("id");
            json::Value* was = v.Find("was");
            if (id && !id->text.empty()) m.banners_.emplace_back(FromUtf8(id->text), was ? std::stoi(was->text) : -1);
        }
    return m;
}

void NotificationMutes::Save() const {
    json::Value root = json::Value::Object();
    json::Value flag;
    flag.type = json::Value::Type::Bool;
    flag.boolean = all;
    root.Member("all") = flag;
    json::Value list = json::Value::Array();
    for (const MutedApp& a : apps) {
        json::Value v = json::Value::Object();
        v.Member("id") = json::Value::String(ToUtf8(a.id));
        v.Member("name") = json::Value::String(ToUtf8(a.name));
        list.items.push_back(std::move(v));
    }
    root.Member("apps") = std::move(list);
    json::Value banners = json::Value::Array();                   // so un-muting can restore them, even after a restart
    for (const auto& [id, was] : banners_) {
        json::Value v = json::Value::Object();
        v.Member("id") = json::Value::String(ToUtf8(id));
        v.Member("was") = json::Value::Number(std::to_string(was));
        banners.items.push_back(std::move(v));
    }
    root.Member("banners_off") = std::move(banners);
    std::ofstream file(StorePath(), std::ios::binary | std::ios::trunc);
    if (file) file << json::Write(root);
}

} // namespace eflow
