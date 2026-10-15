#pragma once
#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Activity.h"

namespace eflow {

// Watches, once a second on a worker thread, for things worth a live pill on the island:
//  * browser downloads in the Downloads folder (Chrome/Edge ".crdownload", Firefox ".part"), growing in size
//  * Explorer copy / move / delete dialogs ("34% complete" in their title)
// Posts `message` to `notify` when the list changes; `finished` lists items that completed since the last Take().
class ActivityMonitor {
public:
    ActivityMonitor(HWND notify, UINT message);
    ~ActivityMonitor();
    ActivityMonitor(const ActivityMonitor&) = delete;
    ActivityMonitor& operator=(const ActivityMonitor&) = delete;

    void Start();
    std::vector<Activity> Snapshot();
    std::vector<std::wstring> TakeFinished();   // "setup.exe" for each download / copy that just completed

private:
    void Loop();
    void ScanDownloads(std::vector<Activity>& out, std::vector<std::wstring>& finished);
    void ScanCopies(std::vector<Activity>& out, std::vector<std::wstring>& finished);

    HWND notify_;
    UINT message_;
    std::wstring downloads_;
    std::thread thread_;
    HANDLE stop_ = nullptr;
    std::mutex mutex_;
    std::vector<Activity> items_;
    std::vector<std::wstring> finished_;
    struct Seen {
        uint64_t size = 0, firstSize = 0;
        uint64_t since = 0;            // tick it was first seen
        std::wstring name;             // the final file name
    };
    std::map<std::wstring, Seen> downloads;   // temp path -> what we know (worker thread only)
    std::map<HWND, std::wstring> copies;      // copy dialogs seen last time (worker thread only)
};

} // namespace eflow
