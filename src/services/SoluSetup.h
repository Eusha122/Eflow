#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eflow {

// One language model Solu could run, with its approximate size so the setup card can show it before downloading.
struct SoluModelChoice {
    std::wstring id;             // the Ollama tag, e.g. "qwen3:4b-instruct"
    std::wstring label;          // "Lighter (4B)"
    std::wstring note;           // "Works on most PCs, including without a dedicated graphics card"
    uint64_t approxBytes = 0;
    bool ready = false;          // already pulled in Ollama
};

// What Begin() would need to do, worked out from this PC: what is already there, what a dedicated GPU can run, and the
// two model choices (a capable one and a lighter one) with which this PC already has. Detect() is quick (file checks,
// one short local request, a GPU memory query) and safe to call on the UI thread.
struct SoluSetupPlan {
    bool pythonReady = false;         // the speech/listening half: faster-whisper, Kokoro, Silero and their packages
    bool speechModelsReady = false;
    bool ollamaInstalled = false;
    bool ollamaRunning = false;
    bool hasNvidiaGpu = false;
    uint64_t vramBytes = 0;
    std::vector<SoluModelChoice> models;   // [0] capable, [1] lighter
    int recommended = 1;                   // index into models: which one this PC should default to
    bool NothingToDo() const { return pythonReady && speechModelsReady && ollamaRunning && recommended >= 0 &&
                                      static_cast<size_t>(recommended) < models.size() && models[static_cast<size_t>(recommended)].ready; }
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

} // namespace eflow
