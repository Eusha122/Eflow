#pragma once
#include <windows.h>

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eflow {

constexpr float kNoValue = std::numeric_limits<float>::quiet_NaN();
inline bool Has(float v) { return !std::isnan(v); }

// Last `kSamples` one-second samples, oldest first.
struct Series {
    static constexpr int kSamples = 60;
    float v[kSamples]{};
    int count = 0;
    void Push(float x) {
        if (count == kSamples) {
            for (int i = 1; i < kSamples; ++i) v[i - 1] = v[i];
            v[kSamples - 1] = x;
        } else {
            v[count++] = x;
        }
    }
    float Latest() const { return count ? v[count - 1] : 0.0f; }
    float Max() const {
        float m = 0;
        for (int i = 0; i < count; ++i) m = v[i] > m ? v[i] : m;
        return m;
    }
};

struct FanReading {
    std::wstring name;
    int rpm = 0;
};

// One point-in-time view of the machine. A plain value, safe to copy across threads.
struct SystemSnapshot {
    bool valid = false;
    ULONGLONG stamp = 0;                         // GetTickCount64() when sampled (graphs scroll smoothly from it)

    std::wstring cpuName;
    int cpuCores = 0, cpuThreads = 0;
    float cpuUsage = 0;                          // percent
    float cpuClockGHz = kNoValue;
    float cpuTempC = kNoValue;                   // needs the optional sensor bridge
    Series cpuHistory;

    bool gpuAvailable = false;
    std::wstring gpuName;
    float gpuUsage = 0;                          // percent
    float gpuTempC = kNoValue, gpuFanPercent = kNoValue, gpuPowerW = kNoValue, gpuClockMHz = kNoValue;
    uint64_t vramUsed = 0, vramTotal = 0;        // bytes (0 = unknown)
    Series gpuHistory;

    uint64_t ramUsed = 0, ramTotal = 0;          // bytes
    float ramUsage = 0;                          // percent
    Series ramHistory;

    double downBps = 0, upBps = 0;               // bits per second, hardware adapters combined
    Series downHistory, upHistory;

    bool sensorBridge = false;                   // LibreHardwareMonitor / OpenHardwareMonitor is running
    std::vector<FanReading> fans;                // from the bridge; the GPU fan is reported separately
};

// Samples CPU, GPU, memory, network and (optionally) temperatures/fans once a second, on a worker
// thread, but only while somebody is watching: Watch(+1) from a visible System tab, Watch(-1) when it
// is left. Posts `message` to `notify` after every sample.
class SystemMonitor {
public:
    SystemMonitor(HWND notify, UINT message);
    ~SystemMonitor();
    SystemMonitor(const SystemMonitor&) = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

    void Start();
    void Watch(int delta);
    SystemSnapshot Snapshot() const;

private:
    struct Sampler;
    void Run();

    HWND notify_;
    UINT message_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    int watchers_ = 0;
    SystemSnapshot snapshot_;
};

} // namespace eflow
