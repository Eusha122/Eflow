#pragma once
#include <filesystem>
#include <string>

namespace eflow {

// Adds or removes Eflow's `PermissionRequest` hook in an AI tool's user-level config. Only Eflow's own
// entry is touched; everything else in the file keeps its order and content. A backup of the
// original is written next to it before the first change.
enum class AiTool { ClaudeCode, Codex };

class HookInstaller {
public:
    static std::filesystem::path ConfigPath(AiTool tool);   // ~/.claude/settings.json, ~/.codex/hooks.json
    static bool IsConnected(AiTool tool);
    static bool Connect(AiTool tool, std::wstring& error);
    static bool Disconnect(AiTool tool, std::wstring& error);
};

} // namespace eflow
