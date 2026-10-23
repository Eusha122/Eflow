#pragma once
#include <filesystem>
#include <string>

namespace eflow {

// Adds or removes Eflow's `PermissionRequest` hook in an AI tool's user-level config. Only Eflow's own
// entry is touched; everything else in the file keeps its order and content. A backup of the
// original is written next to it before the first change.
//
// The tools run a copy of eflow-hook.exe kept at ~/.eflow/hook, a path that survives updates (an installed app's own
// folder changes with every version, and Codex trusts a hook by its exact command). Not under AppData: a Store
// install's writes there are redirected to a private folder the tools can't see.
enum class AiTool { ClaudeCode, Codex };

class HookInstaller {
public:
    static std::filesystem::path ConfigPath(AiTool tool);   // ~/.claude/settings.json, ~/.codex/hooks.json
    static bool IsConnected(AiTool tool);
    static bool IsOutdated(AiTool tool);                      // connected, but with an older command (another folder)
    static bool Connect(AiTool tool, std::wstring& error);    // also brings an outdated entry up to date, in place
    static bool Disconnect(AiTool tool, std::wstring& error);
    // Puts this version's hook at ~/.eflow/hook and records which eflow.exe it may talk to. Call once at startup.
    static void Prepare();
    // The hook command line for `hookExe`, in a form every shell the tools use runs as a program.
    static std::string Command(AiTool tool, const std::filesystem::path& hookExe);
};

} // namespace eflow
