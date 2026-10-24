#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eflow {

// How well a model would sit on this PC. "Comfortable" is the bar for recommending one: it leaves room for the
// games, browsers and editors the same graphics card is already drawing, so Solu never takes the whole card.
enum class SoluModelFit { Comfortable, Tight, TooBig };

// One language model Solu could run, with its approximate size so the setup card can show it before downloading.
struct SoluModelChoice {
    std::wstring id;             // the Ollama tag, e.g. "qwen3:4b-instruct"
    std::wstring label;          // "Lighter · 4B"
    std::wstring note;           // "Works on most PCs, including without a dedicated graphics card"
    uint64_t approxBytes = 0;
    uint64_t needBytes = 0;      // memory it wants while it is answering: its weights plus context and overhead
    bool ready = false;          // already pulled in Ollama
    SoluModelFit fit = SoluModelFit::Comfortable;
    std::wstring fitNote;        // "Fits with room to spare on your 8 GB card"
};

// What Begin() would need to do, worked out from this PC: what is already there, what this PC's memory can carry
// comfortably, and which of the models it already has. Detect() is quick (file checks, one short local request, a
// GPU memory query) and safe to call on the UI thread.
struct SoluSetupPlan {
    bool pythonReady = false;         // the speech/listening half: faster-whisper, Kokoro, Silero and their packages
    bool speechModelsReady = false;
    bool ollamaInstalled = false;
    bool ollamaRunning = false;
    bool hasDedicatedGpu = false;        // a dedicated graphics card was found (any vendor; the name says which)
    uint64_t vramBytes = 0;
    uint64_t ramBytes = 0;
    std::wstring gpuName;
    std::vector<SoluModelChoice> models;   // smallest first, so the list reads from "runs on anything" upwards
    int recommended = 0;                   // index into models: the largest one this PC can carry comfortably
    bool AnyModelReady() const {
        for (const SoluModelChoice& m : models)
            if (m.ready) return true;
        return false;
    }
    bool NothingToDo() const { return pythonReady && speechModelsReady && ollamaRunning && AnyModelReady(); }
    // Rough total download for installing everything still missing, to run `modelId` (its own bytes only if some other
    // model is already pulled; that other model is left alone, never deleted).
    uint64_t EstimateBytes(const std::wstring& modelId) const;
};

enum class SoluSetupStage { Idle, Hardware, PythonRuntime, PythonPackages, SpeechModels, Ollama, OllamaStarting, LanguageModel, Done, Failed };

struct SoluSetupProgress {
    SoluSetupStage stage = SoluSetupStage::Idle;
    std::wstring label;               // "Downloading the language model..."
    uint64_t doneBytes = 0, totalBytes = 0;   // 0, 0 while the size of the current step is not yet known
    bool finished = false;
    bool ok = false;
    std::wstring error;
};

// Turns Solu on for a PC that has never run it: a private Python runtime under solu\pyruntime (never the system
// Python, so this never touches another app's environment), the speech models, Ollama if it is not already on this
// PC, and one language model pulled through Ollama's own local API (the same thing `ollama pull` does, with real
// progress). Every step is skipped if it is already satisfied, so running this again after a partial or failed
// attempt only finishes what is left. Nothing here ever starts without SoluSetup::Begin being called.
class SoluSetup {
public:
    SoluSetup() = default;
    ~SoluSetup();
    SoluSetup(const SoluSetup&) = delete;
    SoluSetup& operator=(const SoluSetup&) = delete;

    SoluSetupPlan Detect() const;
    // Fills in the ladder of models and the recommendation from the memory already in `plan`. Detect() calls it;
    // it is separate so the rule can be exercised against made-up hardware without touching this PC.
    void RateModels(SoluSetupPlan& plan) const;
    void Begin(HWND notify, const std::wstring& modelId);   // posts WM_APP_SOLU_SETUP to `notify` as it goes
    void Cancel();
    bool Running() const { return running_; }
    SoluSetupProgress Snapshot() const;

private:
    void Run(HWND notify, std::wstring modelId);
    void Report(HWND notify, SoluSetupStage stage, const std::wstring& label, uint64_t done = 0, uint64_t total = 0);
    void Finish(HWND notify, bool ok, const std::wstring& error);

    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    SoluSetupProgress snapshot_;
};

// The model the setup last pulled for this PC, or empty when Solu was set up by hand. SoluService passes it to the
// Python side, so a PC that chose the lighter model never tries to answer with one it has not downloaded.
std::wstring SoluChosenModel();

} // namespace eflow
