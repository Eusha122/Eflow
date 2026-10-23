#pragma once
#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eflow {

enum class AgentTool { Claude, Codex };
enum class AgentStatus { Idle, Working, Waiting };

struct AgentEvent {
    enum class Kind { You, Said, Tool, Done, Failed };
    Kind kind = Kind::Said;
    std::wstring text;                  // what was said / the tool's name ("Edit", "Run")
    std::wstring detail;                // the tool's subject ("IslandWindow.cpp", "npm test")
    bool running = false;               // a tool call that has not returned yet
    std::string callId;
    ULONGLONG time = 0;                 // unix milliseconds
};

struct AgentSession {
    AgentTool tool = AgentTool::Claude;
    std::wstring id, project, cwd, title;
    AgentStatus status = AgentStatus::Idle;
    std::wstring doing;                 // "Running npm test", "Thinking..."
    ULONGLONG lastActivity = 0;         // unix milliseconds
    std::vector<AgentEvent> feed;       // oldest first, at most 60
    std::wstring lastReply;             // the agent's final answer of its last turn
};

struct AgentSnapshot {
    std::vector<AgentSession> sessions; // working first, then the most recent
    uint32_t version = 0;               // grows whenever anything changed
    bool claudeFound = false, codexFound = false;       // the programs needed to send prompts exist
    std::wstring sendError;             // the last prompt that could not be delivered
    bool sending = false;               // a prompt is on its way (nothing in the logs yet)
    AgentTool sendingTool = AgentTool::Claude;
    std::wstring justStarted;           // a session started by a prompt from the island: show it
    bool claudeHooked = false, codexHooked = false;     // the island's approval cards are connected (filled in by the app)
};

// Watches the session logs that Claude Code (~/.claude/projects) and Codex (~/.codex/sessions) write and turns them into
// live sessions: what each agent is doing, what it said and ran. Nothing is injected into the tools: the logs are only
// read. It also sends prompts the way you would type them: `claude -p ... --resume <id>` / `codex exec resume <id> ...`
// in the project's folder, with the island's approval hook attached for that run, so approvals come back as cards.
// Posts `message` to `notify` when something changed (coalesced).
class AgentMonitor {
public:
    AgentMonitor(HWND notify, UINT message);
    ~AgentMonitor();
    AgentMonitor(const AgentMonitor&) = delete;
    AgentMonitor& operator=(const AgentMonitor&) = delete;

    void Start();
    AgentSnapshot Snapshot();
    // Sends a prompt. `sessionId` empty (or newSession) starts a new session in `cwd`. Returns false (with `error`) if the
    // agent's program was not found.
    // Brings the window where this session runs to the front: the VS Code (or Cursor...) window that has its project open, or the
    // terminal showing it; Claude sessions are also opened in the Claude panel there, freshly loaded. Returns what it did.
    static std::wstring FocusHost(const AgentSession& session, bool openChat = false);
    bool Send(AgentTool tool, const std::wstring& sessionId, const std::wstring& cwd, const std::wstring& text, bool newSession,
              std::wstring& error);
    std::wstring FindProgram(AgentTool tool) const;   // the agent's program (claude.exe / codex.exe), or empty

private:
    struct Tracked {
        AgentTool tool;
        std::wstring path;
        uint64_t offset = 0;
        AgentSession session;
        bool turnDone = true;
        bool created = false;           // first seen after a prompt was sent from the island
    };
    void Loop();
    void Rescan();
    void Ingest(Tracked& t, const std::string& line);
    void IngestClaude(Tracked& t, const std::string& line);
    void IngestCodex(Tracked& t, const std::string& line);
    void Refresh(Tracked& t, ULONGLONG now);
    void Changed();

    HWND notify_;
    UINT message_;
    std::thread thread_;
    HANDLE stop_ = nullptr;
    std::mutex mutex_;
    std::map<std::wstring, Tracked> tracked_;           // by file path
    std::wstring claudeRoot_, codexRoot_;
    uint32_t version_ = 0;
    std::wstring sendError_, justStarted_;
    bool sending_ = false;
    AgentTool sendingTool_ = AgentTool::Claude;
    ULONGLONG sendingSince_ = 0;
    std::atomic<bool> posted_{false};
    std::vector<std::thread> runners_;
    std::vector<AgentSession> published_;               // what the UI thread reads (copied under the lock)
    bool claudeFound_ = false, codexFound_ = false;
    ULONGLONG claudeCheck_ = 0;
};

} // namespace eflow
