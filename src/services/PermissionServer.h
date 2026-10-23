#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eflow {

// One pending approval request from an AI coding tool, as shown on the island.
struct PermissionRequest {
    uint64_t id = 0;
    std::wstring source;      // "Claude Code", "Codex", ...
    std::wstring tool;        // tool name as the agent reports it ("Bash", "Edit", "apply_patch", ...)
    std::wstring action;      // "run a command", "edit a file", ...
    std::wstring detail;      // the command, file path or other subject, verbatim
    std::wstring project;     // last component of the working directory
    std::wstring cwd;
    bool risky = false;       // matches a destructive-command pattern
    bool canAlways = false;   // Solu: the card may offer "Always allow" for this kind of action
    bool isQuestion = false;  // AskUserQuestion: the card offers one button that goes to the tool's own window
    std::wstring session;     // the tool's session id, when it sends one (Claude Code: matches AgentSession::id)
    ULONGLONG arrived = 0;    // unix ms, to compare against that session's last activity
};

enum class PermissionDecision { Allow, Deny, Defer };

// Named-pipe server for eflow-hook.exe. The pipe is private to the current user and local machine.
// Each request is held open until the user decides (Resolve), the hook gives up, or Eflow exits;
// the list changes are announced with WM_APP_PERMISSION to `notify`.
class PermissionServer {
public:
    explicit PermissionServer(HWND notify);
    ~PermissionServer();
    PermissionServer(const PermissionServer&) = delete;
    PermissionServer& operator=(const PermissionServer&) = delete;

    bool Start();
    std::vector<PermissionRequest> Pending() const;          // oldest first
    void Resolve(uint64_t id, PermissionDecision decision);

private:
    struct Slot;
    void Listen();
    void Serve(HANDLE pipe);

    HWND notify_;
    HANDLE stop_ = nullptr;                                  // manual-reset event
    std::thread listener_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Slot>> slots_;
    struct Client {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Client> clients_;
    uint64_t nextId_ = 1;
};

} // namespace eflow
