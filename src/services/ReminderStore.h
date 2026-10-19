#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/When.h"

namespace eflow {

struct Reminder {
    uint32_t id = 0;
    std::wstring title, note;
    int64_t at = 0;                  // the next time it rings (seconds since 1970)
    Repeat repeat = Repeat::None;
    int earlyMinutes = 0;            // a heads-up this long before `at` (0 = none)
    bool earlyDone = false;          // the heads-up for this `at` was given
    bool missed = false;             // its time passed while Eflow was not running
};

struct Alert {                       // something ringing now
    uint32_t id = 0;
    bool early = false;              // the heads-up, not the reminder itself
    std::wstring title, note;
    int64_t at = 0;
};

struct ReminderSnapshot {
    std::vector<Reminder> items;     // soonest first; missed ones last
    std::vector<Alert> ringing;      // oldest first; the first one is on screen
    uint32_t version = 0;
};

// The reminders and alarms: kept in %LOCALAPPDATA%\Eflow\reminders.json, rung by Tick(), which the app calls from a timer
// on its UI thread (everything here runs on that thread). A reminder whose time passed while the PC was off still rings if
// that was less than 12 hours ago; older ones are marked missed instead of ringing late.
class ReminderStore {
public:
    void Load();
    uint32_t Add(const std::wstring& title, const std::wstring& note, int64_t at, Repeat repeat, int earlyMinutes);
    void Remove(uint32_t id);
    void Dismiss(uint32_t id);                       // stop it ringing (a one-off reminder is done and goes away)
    void Snooze(uint32_t id, int minutes);           // ring again in `minutes`
    int64_t Tick(int64_t now);                       // ring what is due; returns seconds until something is due next
    bool Ringing() const { return !ringing_.empty(); }
    ReminderSnapshot Snapshot() const;

private:
    void Save() const;
    Reminder* Find(uint32_t id);

    std::vector<Reminder> items_;
    std::vector<Alert> ringing_;
    std::vector<uint32_t> done_;                     // one-off reminders that rang: removed once dismissed
    uint32_t nextId_ = 1;
    uint32_t version_ = 0;
};

} // namespace eflow
