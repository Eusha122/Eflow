#pragma once
#include <string>

namespace eflow {

// Windows lists a virtual camera or microphone (a phone used as a webcam, a streaming "virtual camera") as
// present as soon as its driver is installed, whether or not anything is behind it. For the few well-known
// bridges we can do better: the PC app must be running and, for phone apps, it must hold a live network
// connection (that is when a phone is actually streaming to it). Everything else is simply "ready".
struct Readiness {
    bool ready = true;
    std::wstring hint;                   // why not, e.g. "Iriun Webcam isn't running" / "Waiting for your phone"
};

Readiness CheckReadiness(const std::wstring& deviceName);

} // namespace eflow
