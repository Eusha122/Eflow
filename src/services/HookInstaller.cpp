#include "services/HookInstaller.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>

#include "core/Utf8.h"
#include "core/Json.h"
#include "core/Log.h"

namespace eflow {
namespace {

constexpr char kMarker[] = "eflow-hook";           // identifies our entry inside the tool's hook list
constexpr char kTimeoutSeconds[] = "300";

std::filesystem::path Home() {
    PWSTR p = nullptr;
    std::filesystem::path home;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) home = p;
    CoTaskMemFree(p);
    return home;
}

std::filesystem::path ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) { path.resize(n); return path; }
        path.resize(path.size() * 2);
    }
}

std::filesystem::path HookDir() { return Home() / L".eflow" / L"hook"; }

} // namespace

// The tools hand this to a shell: Codex to PowerShell, Claude Code to Git Bash or cmd. A quoted path is only a string
// in PowerShell (the hook would never start), so the path goes unquoted, with forward slashes, and in its short (8.3)
// form when it has spaces. Only a drive without short names gets quotes.
std::string HookInstaller::Command(AiTool tool, const std::filesystem::path& hookExe) {
    std::wstring hook = hookExe.wstring();
    if (hook.find(L' ') != std::wstring::npos) {
        wchar_t shortPath[MAX_PATH];
        const DWORD n = GetShortPathNameW(hook.c_str(), shortPath, MAX_PATH);
        if (n && n < MAX_PATH) hook.assign(shortPath, n);
    }
    std::string path = ToUtf8(hook);
    for (char& c : path)
        if (c == '\\') c = '/';
    if (path.find(' ') != std::string::npos) path = "\"" + path + "\"";
    return path + " --source " + (tool == AiTool::ClaudeCode ? "claude-code" : "codex");
}

namespace {

std::string HookCommand(AiTool tool) { return HookInstaller::Command(tool, HookDir() / L"eflow-hook.exe"); }

// entry = { "matcher"?: ..., "hooks": [ { "type": "command", "command": "...eflow-hook..." } ] }
json::Value* OurHook(json::Value& entry) {
    json::Value* hooks = entry.Find("hooks");
    if (!hooks || hooks->type != json::Value::Type::Array) return nullptr;
    for (auto& h : hooks->items) {
        json::Value* cmd = h.type == json::Value::Type::Object ? h.Find("command") : nullptr;
        if (cmd && cmd->type == json::Value::Type::String && cmd->text.find(kMarker) != std::string::npos) return &h;
    }
    return nullptr;
}

bool IsOurs(json::Value& entry) { return OurHook(entry) != nullptr; }

std::string ReadAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Writes `text` to `file` only when it differs, through a temporary file so a reader never sees half of it.
void WriteIfChanged(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && ReadAll(file) == text) return;
    const std::filesystem::path temp = file.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(temp.c_str());
}

// The PermissionRequest list of a loaded config, or null.
json::Value* PermissionList(json::Value& root) {
    json::Value* hooks = root.Find("hooks");
    json::Value* list = hooks && hooks->type == json::Value::Type::Object ? hooks->Find("PermissionRequest") : nullptr;
    return list && list->type == json::Value::Type::Array ? list : nullptr;
}

bool Load(const std::filesystem::path& file, json::Value& root, std::wstring& error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        root = json::Value::Object();
        return true;
    }
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string parseError;
    const std::string text = ss.str();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        root = json::Value::Object();
        return true;
    }
    if (!json::Parse(text, root, parseError) || root.type != json::Value::Type::Object) {
        // Never overwrite a file we cannot understand.
        error = L"Could not read " + file.wstring() + L" (" + FromUtf8(parseError) + L"). It was left unchanged.";
        return false;
    }
    return true;
}

bool Save(const std::filesystem::path& file, const json::Value& root, std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path backup = file.wstring() + L".eflow-backup";
    if (std::filesystem::exists(file, ec) && !std::filesystem::exists(backup, ec))
        std::filesystem::copy_file(file, backup, ec);          // keep the user's original once

    const std::filesystem::path temp = file.wstring() + L".eflow-tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << json::Write(root);
        if (!out) {
            error = L"Could not write " + temp.wstring();
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"Could not replace " + file.wstring();
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path HookInstaller::ConfigPath(AiTool tool) {
    return tool == AiTool::ClaudeCode ? Home() / L".claude" / L"settings.json" : Home() / L".codex" / L"hooks.json";
}

bool HookInstaller::IsConnected(AiTool tool) {
    json::Value root;
    std::wstring error;
    if (!Load(ConfigPath(tool), root, error)) return false;
    json::Value* list = PermissionList(root);
    if (!list) return false;
    for (auto& entry : list->items)
        if (entry.type == json::Value::Type::Object && IsOurs(entry)) return true;
    return false;
}

bool HookInstaller::IsOutdated(AiTool tool) {
    json::Value root;
    std::wstring error;
    if (!Load(ConfigPath(tool), root, error)) return false;
    json::Value* list = PermissionList(root);
    if (!list) return false;
    const std::string current = HookCommand(tool);
    for (auto& entry : list->items)
        if (json::Value* h = entry.type == json::Value::Type::Object ? OurHook(entry) : nullptr)
            if (h->Find("command")->text != current) return true;
    return false;
}

bool HookInstaller::Connect(AiTool tool, std::wstring& error) {
    const auto file = ConfigPath(tool);
    json::Value root;
    if (!Load(file, root, error)) return false;
    json::Value& hooks = root.Member("hooks");
    if (hooks.type != json::Value::Type::Object) hooks = json::Value::Object();
    json::Value& list = hooks.Member("PermissionRequest");
    if (list.type != json::Value::Type::Array) list = json::Value::Array();

    // Already there (perhaps from an older version): update that entry where it is and drop any duplicates.
    size_t first = SIZE_MAX;
    for (size_t i = 0; i < list.items.size();) {
        const bool ours = list.items[i].type == json::Value::Type::Object && IsOurs(list.items[i]);
        if (ours && first != SIZE_MAX) list.items.erase(list.items.begin() + static_cast<std::ptrdiff_t>(i));
        else {
            if (ours) first = i;
            ++i;
        }
    }
    if (first < list.items.size()) {
        json::Value* h = OurHook(list.items[first]);
        h->Member("command") = json::Value::String(HookCommand(tool));
        h->Member("timeout") = json::Value::Number(kTimeoutSeconds);
    } else {
        // No matcher: every tool's permission prompt is routed to the island.
        json::Value command = json::Value::Object();
        command.Member("type") = json::Value::String("command");
        command.Member("command") = json::Value::String(HookCommand(tool));
        command.Member("timeout") = json::Value::Number(kTimeoutSeconds);
        json::Value entry = json::Value::Object();
        entry.Member("hooks") = json::Value::Array();
        entry.Member("hooks").items.push_back(std::move(command));
        list.items.push_back(std::move(entry));
    }

    if (!Save(file, root, error)) return false;
    Log("ai: connected %s (%ls)", tool == AiTool::ClaudeCode ? "Claude Code" : "Codex", file.c_str());
    return true;
}

void HookInstaller::Prepare() {
    std::error_code ec;
    const std::filesystem::path dir = HookDir(), bundled = ExePath().parent_path() / L"eflow-hook.exe", hook = dir / L"eflow-hook.exe";
    std::filesystem::create_directories(dir, ec);
    const std::string bytes = ReadAll(bundled);
    if (bytes.empty()) {
        Log("ai: %ls is missing; approvals from the island won't work", bundled.c_str());
        return;
    }
    const std::filesystem::path old = hook.wstring() + L".old";
    DeleteFileW(old.c_str());
    if (!std::filesystem::exists(hook, ec) || ReadAll(hook) != bytes) {
        MoveFileExW(hook.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);   // a hook that is running right now can be renamed, not replaced
        if (CopyFileW(bundled.c_str(), hook.c_str(), FALSE)) Log("ai: updated %ls", hook.c_str());
        else Log("ai: could not copy the hook to %ls (%lu)", hook.c_str(), GetLastError());
    }
    // The hook only answers through this eflow.exe (see src/hook/main.cpp).
    WriteIfChanged(dir / L"eflow-path.txt", ToUtf8(ExePath().wstring()));
}

bool HookInstaller::Disconnect(AiTool tool, std::wstring& error) {
    const auto file = ConfigPath(tool);
    json::Value root;
    if (!Load(file, root, error)) return false;
    json::Value* hooks = root.Find("hooks");
    json::Value* list = hooks && hooks->type == json::Value::Type::Object ? hooks->Find("PermissionRequest") : nullptr;
    if (!list || list->type != json::Value::Type::Array) return true;
    std::erase_if(list->items, [](json::Value& e) { return e.type == json::Value::Type::Object && IsOurs(e); });
    if (list->items.empty()) std::erase_if(hooks->members, [](auto& m) { return m.first == "PermissionRequest"; });
    if (hooks->members.empty()) std::erase_if(root.members, [](auto& m) { return m.first == "hooks"; });
    if (!Save(file, root, error)) return false;
    Log("ai: disconnected %s", tool == AiTool::ClaudeCode ? "Claude Code" : "Codex");
    return true;
}

} // namespace eflow
