#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eflow {

struct AudioDevice {
    std::wstring id, name;                       // Windows endpoint id, friendly name ("Microphone (Iriun Webcam)")
    bool isDefault = false;                      // the system's default for this direction
    bool ready = true;                           // usable right now (a phone-based device needs its phone connected)
    std::wstring hint;                           // why not ready, e.g. "Waiting for your phone"
    int form = 10;                               // EndpointFormFactor: 1 speakers, 3 headphones, 5 headset, 9 monitor (HDMI/DP), 8 S/PDIF ...
    std::vector<std::wstring> apps;              // apps with an active audio stream on this device (outputs; refreshed while watched)
};

// One direction (microphones, or speakers/headphones).
struct AudioSide {
    std::vector<AudioDevice> devices;            // plugged in and enabled, as Windows lists them
    std::wstring selectedId;                     // the user's pick; empty = follow the system default
    std::wstring name;                           // the device in use (the default's name when following it)
    bool connected = false;                      // that device is present right now
    bool ready = false;                          // ... and usable (see AudioDevice::ready)
    std::wstring hint;
    std::vector<std::wstring> inUseBy;           // apps with an active audio stream on it right now
    float level = 0;                             // 0..1 peak of what it is carrying (only sampled while watched)
    bool muted = false;                          // the device itself is muted (the same mute as Windows' volume panel)
};

// A user-started check that the microphone hears something / the speaker plays something.
struct AudioTest {
    enum class State { Idle, Running, Done } state = State::Idle;
    enum class Verdict { None, Works, Quiet, Silent, Blocked, Error, Muted } verdict = Verdict::None;
    bool input = true;                           // which side
    float level = 0;                             // live 0..1
    float peak = 0;                              // loudest so far
    ULONGLONG doneAt = 0;                        // GetTickCount64() when it finished
};

struct AudioSnapshot {
    bool valid = false;
    AudioSide input, output;
    AudioTest test;
};

// Lists the audio devices, tells when one comes or goes (event-driven, no polling) and remembers which
// ones Solu should use in %LOCALAPPDATA%\Eflow\voice.json. Picking a device here never changes the
// system default: the choice lives in that file, which Solu reads and follows. While somebody watches
// (Watch(+1) while the island is open) the peak level, mute state, readiness and the apps using each device
// are refreshed several times a second. Posts `message` to `notify` whenever the snapshot changes.
class AudioDevices {
public:
    AudioDevices(HWND notify, UINT message);
    ~AudioDevices();
    AudioDevices(const AudioDevices&) = delete;
    AudioDevices& operator=(const AudioDevices&) = delete;

    void Start();
    void Watch(int delta);
    void Select(bool input, const std::wstring& id);     // empty id = follow the system default
    void SetMute(bool input, bool mute);                 // mutes/unmutes the device in use (Windows endpoint mute)
    // Picking a device makes it Windows' default (all roles), so Spotify, Chrome, Discord... follow it.
    // Only ever started by the user pressing "Test": the microphone listens for ~5 s (nothing is recorded or
    // kept; only the loudness is looked at), the speaker plays a short chime.
    void StartTest(bool input);
    void StopTest();
    AudioSnapshot Snapshot() const;

private:
    void Run();
    void TestRun(bool input, std::wstring id, bool muted);
    void LoadSelection();
    void SaveSelectionLocked() const;

    HWND notify_;
    UINT message_;
    std::thread worker_, testThread_;
    std::atomic<bool> testStop_{false};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool refresh_ = true;
    int watchers_ = 0;
    std::wstring pendingDefault_[2];             // device id to make the Windows default, applied on the worker thread
    int mutePending_[2] = {-1, -1};              // -1 nothing, 0 unmute, 1 mute: applied on the worker thread
    std::wstring selId_[2], selName_[2];         // [0] input, [1] output; last known name survives unplugging
    AudioSnapshot snapshot_;
};

} // namespace eflow
