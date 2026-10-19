#include "services/ReminderStore.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/Json.h"
#include "core/Utf8.h"

namespace eflow {
namespace {

constexpr int64_t kLateLimit = 12 * 3600;            // rang while the PC was off: still worth ringing if this recent
constexpr int64_t kMaxWait = 60;                     // Tick at least once a minute (the clock may jump: sleep, time zone)

std::wstring StorePath() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    dir += L"\\Eflow";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\reminders.json";
}

const char* RepeatName(Repeat r) {
    switch (r) {
    case Repeat::Daily: return "daily";
    case Repeat::Weekdays: return "weekdays";
    case Repeat::Weekly: return "weekly";
    default: return "none";
    }
}

Repeat RepeatFrom(const std::string& s) {
    return s == "daily" ? Repeat::Daily : s == "weekdays" ? Repeat::Weekdays : s == "weekly" ? Repeat::Weekly : Repeat::None;
}

json::Value Number(int64_t n) { return json::Value::Number(std::to_string(n)); }

json::Value Bool(bool b) {
    json::Value v;
    v.type = json::Value::Type::Bool;
    v.boolean = b;
    return v;
}

}  // namespace

void ReminderStore::Load() {
    std::ifstream file(StorePath(), std::ios::binary);
    if (!file) return;
    std::stringstream text;
    text << file.rdbuf();
    json::Value root;
    std::string error;
    if (!json::Parse(text.str(), root, error) || root.type != json::Value::Type::Object) return;
    json::Value* list = root.Find("reminders");
    if (!list || list->type != json::Value::Type::Array) return;
    for (json::Value& v : list->items) {
        if (v.type != json::Value::Type::Object) continue;
        auto str = [&](const char* key) { json::Value* f = v.Find(key); return f ? f->text : std::string(); };
        auto num = [&](const char* key) { const std::string s = str(key); return s.empty() ? 0LL : std::stoll(s); };
        auto flag = [&](const char* key) { json::Value* f = v.Find(key); return f && f->type == json::Value::Type::Bool && f->boolean; };
        Reminder r;
        r.id = static_cast<uint32_t>(num("id"));
        r.title = FromUtf8(str("title"));
        r.note = FromUtf8(str("note"));
        r.at = num("at");
        r.repeat = RepeatFrom(str("repeat"));
        r.earlyMinutes = static_cast<int>(num("early"));
        r.earlyDone = flag("early_done");
        r.missed = flag("missed");
        if (r.id == 0 || r.at == 0) continue;
        nextId_ = std::max(nextId_, r.id + 1);
        items_.push_back(std::move(r));
    }
    ++version_;
}

void ReminderStore::Save() const {
    json::Value root = json::Value::Object();
    json::Value list = json::Value::Array();
    for (const Reminder& r : items_) {
        if (std::find(done_.begin(), done_.end(), r.id) != done_.end()) continue;   // rang and waits for Dismiss: not kept
        json::Value v = json::Value::Object();
        v.Member("id") = Number(r.id);
        v.Member("title") = json::Value::String(ToUtf8(r.title));
        v.Member("note") = json::Value::String(ToUtf8(r.note));
        v.Member("at") = Number(r.at);
        v.Member("repeat") = json::Value::String(RepeatName(r.repeat));
        v.Member("early") = Number(r.earlyMinutes);
        v.Member("early_done") = Bool(r.earlyDone);
        v.Member("missed") = Bool(r.missed);
        list.items.push_back(std::move(v));
    }
    root.Member("reminders") = std::move(list);
    const std::wstring path = StorePath(), tmp = path + L".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << json::Write(root);
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);   // never a half-written file
}

Reminder* ReminderStore::Find(uint32_t id) {
    auto it = std::find_if(items_.begin(), items_.end(), [id](const Reminder& r) { return r.id == id; });
    return it == items_.end() ? nullptr : &*it;
}

uint32_t ReminderStore::Add(const std::wstring& title, const std::wstring& note, int64_t at, Repeat repeat, int earlyMinutes) {
    Reminder r;
    r.id = nextId_++;
    r.title = title.empty() ? L"Reminder" : title;
    r.note = note;
    r.at = at;
    r.repeat = repeat;
    r.earlyMinutes = std::max(0, earlyMinutes);
    r.earlyDone = earlyMinutes > 0 && at - earlyMinutes * 60LL <= UnixNow();   // too late for the heads-up already
    items_.push_back(std::move(r));
    ++version_;
    Save();
    return items_.back().id;
}

void ReminderStore::Remove(uint32_t id) {
    std::erase_if(items_, [id](const Reminder& r) { return r.id == id; });
    std::erase_if(ringing_, [id](const Alert& a) { return a.id == id; });
    std::erase(done_, id);
    ++version_;
    Save();
}

void ReminderStore::Dismiss(uint32_t id) {
    std::erase_if(ringing_, [id](const Alert& a) { return a.id == id; });
    if (std::find(done_.begin(), done_.end(), id) != done_.end()) {     // a one-off reminder that rang: it is finished
        std::erase_if(items_, [id](const Reminder& r) { return r.id == id; });
        std::erase(done_, id);
    }
    ++version_;
    Save();
}

void ReminderStore::Snooze(uint32_t id, int minutes) {
    const auto alert = std::find_if(ringing_.begin(), ringing_.end(), [id](const Alert& a) { return a.id == id; });
    if (alert == ringing_.end()) return;
    const Alert copy = *alert;
    ringing_.erase(alert);
    const int64_t when = UnixNow() + minutes * 60LL;
    Reminder* r = Find(id);
    if (r && std::find(done_.begin(), done_.end(), id) != done_.end()) {
        r->at = when;                                                    // a one-off reminder: it just moves
        std::erase(done_, id);
    } else if (!copy.early) {
        Add(copy.title, copy.note, when, Repeat::None, 0);               // a repeating one keeps its schedule; this is extra
        return;
    }
    ++version_;
    Save();
}

int64_t ReminderStore::Tick(int64_t now) {
    bool changed = false;
    int64_t next = kMaxWait;
    for (Reminder& r : items_) {
        if (r.missed || std::find(done_.begin(), done_.end(), r.id) != done_.end()) continue;
        if (r.earlyMinutes > 0 && !r.earlyDone) {
            const int64_t heads = r.at - r.earlyMinutes * 60LL;
            if (now >= heads && now < r.at) {
                ringing_.push_back({r.id, true, r.title, r.note, r.at});
                r.earlyDone = changed = true;
            } else if (now < heads) {
                next = std::min(next, heads - now);
            }
        }
        if (now < r.at) {
            next = std::min(next, r.at - now);
            continue;
        }
        changed = true;
        const bool late = now - r.at > kLateLimit;
        if (!late) {
            std::erase_if(ringing_, [&](const Alert& a) { return a.id == r.id; });   // the heads-up gives way to the real thing
            ringing_.push_back({r.id, false, r.title, r.note, r.at});
        }
        if (r.repeat != Repeat::None) {
            r.at = NextOccurrence(r.at, r.repeat, now);
            r.earlyDone = r.earlyMinutes > 0 && r.at - r.earlyMinutes * 60LL <= now;
            next = std::min(next, r.at - now);
        } else if (late) {
            r.missed = true;
        } else {
            done_.push_back(r.id);
        }
    }
    if (changed) {
        ++version_;
        Save();
    }
    return std::max<int64_t>(1, next);
}

ReminderSnapshot ReminderStore::Snapshot() const {
    ReminderSnapshot s;
    for (const Reminder& r : items_)
        if (std::find(done_.begin(), done_.end(), r.id) == done_.end()) s.items.push_back(r);
    std::stable_sort(s.items.begin(), s.items.end(), [](const Reminder& a, const Reminder& b) {
        return a.missed != b.missed ? b.missed : a.at < b.at;
    });
    s.ringing = ringing_;
    s.version = version_;
    return s;
}

} // namespace eflow
