#pragma once
#include <string>

namespace eflow {

// Marks Eflow's PermissionRequest hook as trusted in Codex, exactly as `/hooks` does inside Codex: Codex's own
// app-server lists the hooks with the hash it expects, and that hash is recorded for Eflow's hook only. Codex skips
// hooks nobody has trusted, so without this the island never sees Codex's requests. Only call it once the user has
// agreed to connect Codex. Blocking (starts Codex, up to ~20 s): call it off the UI thread.
enum class CodexTrust { Trusted, AlreadyTrusted, NoCodex, NoHook, Failed };

// `program`: codex.exe as found by AgentMonitor, or empty (then codex.exe / codex.cmd on PATH).
CodexTrust TrustEflowHookInCodex(std::wstring program, std::wstring& detail);

} // namespace eflow
