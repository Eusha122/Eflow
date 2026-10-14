#pragma once
#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eflow {

struct CameraDevice {
    std::wstring name;
    bool ready = true;                           // usable now (a phone-based camera needs its phone connected)
    std::wstring hint;                           // why not, e.g. "Waiting for your phone"
    bool operator==(const CameraDevice&) const = default;
};

struct CameraSnapshot {
    bool valid = false;
    std::vector<CameraDevice> devices;           // the cameras that are plugged in
    std::vector<std::wstring> inUseBy;           // friendly names of the apps using a camera right now
    bool blocked = false;                        // Windows' camera privacy switch is off for this user
};

// Cameras, what is using them, and Windows' own "camera access" privacy switch. Windows has no way to mute
// a webcam from a normal program (disabling the device needs administrator rights), so "block" flips the same
// per-user switch as Settings > Privacy > Camera ("Let apps / desktop apps access your camera"): no app can
// open the camera until it is switched back. Read once a second while somebody watches (Watch(+1) while the
// island is open); posts `message` to `notify` when the snapshot changes.
class CameraService {
public:
    CameraService(HWND notify, UINT message);
    ~CameraService();
    CameraService(const CameraService&) = delete;
    CameraService& operator=(const CameraService&) = delete;

    void Start();
    void Watch(int delta);
    void SetBlocked(bool blocked);
    CameraSnapshot Snapshot() const;

private:
    void Run();

    HWND notify_;
    UINT message_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool refresh_ = true;
    int watchers_ = 0;
    int blockPending_ = -1;                      // -1 nothing, 0 allow, 1 deny
    CameraSnapshot snapshot_;
};

} // namespace eflow
