#pragma once
#include <windows.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Image.h"
#include "services/PermissionServer.h"

namespace eflow {

// Solu's approval requests share the island's approval card with the coding tools; their ids live up here.
constexpr uint64_t kSoluApprovalBase = 1ull << 40;

enum class SoluState { Off, Missing, Loading, Ready, Listening, Thinking, Speaking };

struct SoluVoice {
    const wchar_t* id;                           // what the helper understands
    const wchar_t* name;                         // shown in the menu
    const wchar_t* description;
};
extern const SoluVoice kSoluVoices[5];

// An event from Solu that the app itself acts on: "activity" / "activity_end" (a live pill), "nudge", "focus",
// "capture" (the user wants to show Solu part of the screen), "cheer". Fields are the event's JSON members as text.
struct SoluAppEvent {
    std::string name;
    std::map<std::string, std::wstring> fields;
    std::wstring Get(const std::string& key) const {
        auto it = fields.find(key);
        return it == fields.end() ? L"" : it->second;
    }
};

struct SoluExchange {
    std::wstring heard, said;
};

struct SoluTask {                              // a long task Solu is working on (or has finished)
    std::string id;
    std::wstring title, status, step, question, result;
    int done = 0, total = 0;
};

struct SoluSnapshot {
    SoluState state = SoluState::Off;
    bool enabled = false;                        // the user switched Solu on
    std::wstring voice = L"cozy";
    bool headphones = false;                     // headphones: you may talk over Solu; speakers: it never hears itself
    std::wstring heard, said;                    // the last thing each side said
    std::vector<SoluExchange> history;           // the last few exchanges, oldest first (the Solu AI tab)
    bool captions = true;                        // show the live conversation view (Solu can switch this by voice)
    float mic = 0;                               // how loudly somebody is talking right now, 0..1 (the thin bar reacts)
    bool screen = true;                          // "Solu, look at this" is allowed (Solu can switch this by voice)
    std::wstring lookTitle;                      // what Solu is looking at for the current question
    std::shared_ptr<const ImageBGRA> lookImage;  // a small picture of it, shown so it is always clear what was seen
    std::vector<SoluTask> tasks;                 // newest first
    std::vector<PermissionRequest> approvals;     // things Solu wants to do on the PC, waiting for the user
    uint32_t approvalsVersion = 0;               // grows whenever that list changes
    bool sleepRequested = false;                 // "Solu, go to sleep": the app switches Solu off
    std::wstring error;                          // something the user should know ("Ollama is not running")
    float level = 0;                             // how loudly Solu is speaking right now, 0..1 (the cat's mouth)
    uint32_t turn = 0;                           // grows with every new heard/said line
};

// Runs Solu (the voice assistant) as a hidden helper process and talks to it over its stdin/stdout: one JSON
// line each way. Solu is off until the user switches it on; then it loads its models (a few seconds to a
// minute) and waits with the microphone CLOSED. The microphone opens only for a conversation the user starts
// (hotkey / button) and closes when it ends. The helper lives in a job object, so it dies with Eflow.
// Posts `message` to `notify` when the snapshot changes (level updates are coalesced).
class SoluService {
public:
    SoluService(HWND notify, UINT message);
    ~SoluService();
    SoluService(const SoluService&) = delete;
    SoluService& operator=(const SoluService&) = delete;

    void Start();                                // reads the saved settings; starts Solu if it was on
    void Enable(bool on);
    void Talk();                                 // starts a conversation, or ends the running one; starts Solu first if needed
    void StopSpeaking();
    void SetVoice(const std::wstring& id);
    void SetHeadphones(bool on);
    void SetCaptions(bool on);
    void SetScreen(bool on);
    void Answer(uint64_t id, PermissionDecision decision);   // the approval card: Approve / Reject / Always allow (Defer)
    void Ask(const std::wstring& text);
    void TaskControl(const std::string& op, const std::string& id);   // pause / resume / cancel / remove
    void Say(const std::wstring& text);          // Solu says this line (timers, focus sessions)
    void SetPaused(bool paused);                 // privacy: the microphone closes and Solu goes quiet until resumed
    void LookAtRegion(RECT screen, const std::wstring& mode);   // quick capture: explain / translate this part of the screen
    void SendContext(const std::string& json);   // what Eflow knows that Solu may use (notification counts)
    void ReminderReply(const std::wstring& said);   // the answer to a "reminder" event: what Solu should say
    std::vector<SoluAppEvent> TakeAppEvents();          // answers a typed question (tests / future text box)
    SoluSnapshot Snapshot();
    std::vector<PermissionRequest> Approvals();

private:
    bool Launch();
    void ReaderLoop(HANDLE process, HANDLE readPipe);
    void Send(const std::string& json);
    void Kill();
    void LoadSettings();
    void SaveSettings() const;
    void Changed(bool coalesce);
    std::wstring FindDir() const;

    HWND notify_;
    UINT message_;
    std::mutex mutex_;
    SoluSnapshot snapshot_;
    HANDLE process_ = nullptr, writePipe_ = nullptr, job_ = nullptr;
    std::thread reader_;
    bool talkWhenReady_ = false;
    std::wstring pendingHeard_;
    std::vector<SoluAppEvent> appEvents_;
    std::atomic<bool> posted_{false};
};

} // namespace eflow
