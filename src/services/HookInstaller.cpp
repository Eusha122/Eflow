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

// The command line the tool runs. Forward slashes work for every shell the tools may use on
// Windows (cmd, PowerShell, Git Bash), and the quotes protect paths with spaces.
std::string HookCommand(AiTool tool) {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::filesystem::path hook = std::filesystem::path(std::wstring(exe, n)).parent_path() / L"eflow-hook.exe";
    std::string path = ToUtf8(hook.wstring());
    for (char& c : path)
        if (c == '\\') c = '/';
    return "\"" + path + "\" --source " + (tool == AiTool::ClaudeCode ? "claude-code" : "codex");
}

bool IsOurs(json::Value& entry) {
    // entry = { "matcher"?: ..., "hooks": [ { "type": "command", "command": "...eflow-hook..." } ] }
    json::Value* hooks = entry.Find("hooks");
    if (!hooks || hooks->type != json::Value::Type::Array) return false;
    for (auto& h : hooks->items) {
        json::Value* cmd = h.type == json::Value::Type::Object ? h.Find("command") : nullptr;
        if (cmd && cmd->type == json::Value::Type::String && cmd->text.find(kMarker) != std::string::npos) return true;
    }
    return false;
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
    json::Value* hooks = root.Find("hooks");
    json::Value* list = hooks && hooks->type == json::Value::Type::Object ? hooks->Find("PermissionRequest") : nullptr;
    if (!list || list->type != json::Value::Type::Array) return false;
    for (auto& entry : list->items)
        if (entry.type == json::Value::Type::Object && IsOurs(entry)) return true;
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
    std::erase_if(list.items, [](json::Value& e) { return e.type == json::Value::Type::Object && IsOurs(e); });

    // No matcher: every tool's permission prompt is routed to the island.
    json::Value command = json::Value::Object();
    command.Member("type") = json::Value::String("command");
    command.Member("command") = json::Value::String(HookCommand(tool));
    command.Member("timeout") = json::Value::Number(kTimeoutSeconds);
    json::Value entry = json::Value::Object();
    entry.Member("hooks") = json::Value::Array();
    entry.Member("hooks").items.push_back(std::move(command));
    list.items.push_back(std::move(entry));

    if (!Save(file, root, error)) return false;
    Log("ai: connected %s (%ls)", tool == AiTool::ClaudeCode ? "Claude Code" : "Codex", file.c_str());
    return true;
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
