// Shoot mode (debug builds only): the launch video is filmed on the real island, with the scenes' content fed
// in by a director script (build/shoot/director.py) over WM_COPYDATA, one JSON command per message. While it is
// on, the real notification, agent, Solu, permission, system, device, shelf and activity feeds are ignored and
// the island's buttons only change the scripted state (nothing is really muted, approved or sent). Music stays real.
#include "App.h"

#ifndef NDEBUG
#include <fstream>
#include <iterator>

#include "core/Utf8.h"
#include "core/Json.h"
#include "core/Log.h"

namespace eflow {
namespace {

using json::Value;

const Value* Get(const Value& v, const char* key) {
    return const_cast<Value&>(v).Find(key);
}
bool Has(const Value& v, const char* key) { return Get(v, key) != nullptr; }
std::wstring Str(const Value& v, const char* key, const wchar_t* fallback = L"") {
    const Value* m = Get(v, key);
    return m && m->type == Value::Type::String ? FromUtf8(m->text) : std::wstring(fallback);
}
double Num(const Value& v, const char* key, double fallback = 0) {
    const Value* m = Get(v, key);
    return m && m->type == Value::Type::Number ? atof(m->text.c_str()) : fallback;
}
bool Bool(const Value& v, const char* key, bool fallback = false) {
    const Value* m = Get(v, key);
    return m && m->type == Value::Type::Bool ? m->boolean : fallback;
}
const std::vector<Value>& Items(const Value& v, const char* key) {
    static const std::vector<Value> none;
    const Value* m = Get(v, key);
    return m && m->type == Value::Type::Array ? m->items : none;
}

std::shared_ptr<const ImageBGRA> LoadPicture(const std::wstring& path, uint32_t maxSide) {
    if (path.empty()) return nullptr;
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto image = std::make_shared<ImageBGRA>();
    if (bytes.empty() || !DecodeImage(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), 1.0f, maxSide, *image)) {
        Log("shoot: could not load %ls", path.c_str());
        return nullptr;
    }
    return image;
}

ULONGLONG UnixMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000ull) / 10000;
}

PermissionRequest ParsePermission(const Value& v, uint64_t id) {
    PermissionRequest r;
    r.id = id;
    r.source = Str(v, "source", L"Claude Code");
    r.tool = Str(v, "tool", L"Bash");
    r.action = Str(v, "action", L"run a command");
    r.detail = Str(v, "detail");
    r.project = Str(v, "project");
    r.cwd = Str(v, "cwd");
    r.risky = Bool(v, "risky");
    r.canAlways = Bool(v, "canAlways");
    return r;
}

AgentEvent::Kind EventKind(const std::wstring& k) {
    if (k == L"you") return AgentEvent::Kind::You;
    if (k == L"tool") return AgentEvent::Kind::Tool;
    if (k == L"done") return AgentEvent::Kind::Done;
    if (k == L"failed") return AgentEvent::Kind::Failed;
    return AgentEvent::Kind::Said;
}

AudioSide ParseSide(const Value& v, int defaultForm) {
    AudioSide side;
    for (const Value& d : Items(v, "devices")) {
        AudioDevice dev;
        dev.name = Str(d, "name");
        dev.id = dev.name;
        dev.form = static_cast<int>(Num(d, "form", defaultForm));
        dev.isDefault = Bool(d, "default");
        for (const Value& app : Items(d, "apps")) dev.apps.push_back(FromUtf8(app.text));
        side.devices.push_back(std::move(dev));
    }
    side.name = Str(v, "name", side.devices.empty() ? L"" : side.devices.front().name.c_str());
    side.selectedId = Str(v, "selected");
    side.connected = side.ready = true;
    for (const Value& a : Items(v, "inUseBy")) side.inUseBy.push_back(FromUtf8(a.text));
    side.level = static_cast<float>(Num(v, "level"));
    side.muted = Bool(v, "muted");
    return side;
}

} // namespace

void App::OnShootCommand(const std::string& text) {
    Value v;
    std::string error;
    if (!json::Parse(text, v, error) || v.type != Value::Type::Object) {
        Log("shoot: bad command (%s)", error.c_str());
        return;
    }
    const std::wstring cmd = Str(v, "cmd");
    if (cmd == L"shoot") {
        const bool on = Bool(v, "on", true);
        if (on == shoot_) return;
        shoot_ = on;
        Log("shoot mode %s", on ? "on" : "off");
        if (on) {
            shootNotifs_ = {};
            shootNotifs_.access = NotificationAccess::Allowed;
            shootPerms_.clear();
            shootAgents_ = {};
            shootAgents_.claudeFound = shootAgents_.codexFound = true;
            shootAgents_.claudeHooked = shootAgents_.codexHooked = true;
            shootSolu_ = {};
            shootSolu_.enabled = true;
            shootSolu_.state = SoluState::Ready;
            shootActs_.clear();
            shootShelf_ = {};
            shootSystem_ = {};
            shootAudio_ = {};
            shootCamera_ = {};
            for (auto& island : islands_) island->DebugOpenTab(-1);
            ShootPushAll();
        } else {                                                  // back to the real world
            OnNotificationsChanged();
            OnAgentsChanged();
            OnSoluChanged();
            OnPermissionsChanged();
            OnSystemChanged();
            OnAudioChanged();
            OnCameraChanged();
            OnShelfChanged();
            PushActivities();
        }
        return;
    }
    if (!shoot_) return;
    const uint64_t now = GetTickCount64();

    if (cmd == L"notify") {                                       // a message / notification arrives
        NotificationItem n;
        n.id = ++shootSeq_;
        n.app = Str(v, "app", L"WhatsApp");
        n.appId = Str(v, "appId", L"5319275A.WhatsAppDesktop_cv1g1gvanyjgm!App");
        n.title = Str(v, "title");
        n.body = Str(v, "body");
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        n.time = static_cast<ULONGLONG>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime;
        n.icon = LoadPicture(Str(v, "icon"), 128);
        shootNotifs_.items.insert(shootNotifs_.items.begin(), std::move(n));
        ++shootNotifs_.arrivals;
        const bool quiet = Bool(v, "quiet");                      // set up the list without the island opening
        for (auto& island : islands_) {
            island->SetNotifications(shootNotifs_);
            if (!quiet) island->OnNotification(1);
        }
    } else if (cmd == L"notifs_clear") {
        shootNotifs_.items.clear();
        for (auto& island : islands_) island->SetNotifications(shootNotifs_);
    } else if (cmd == L"permission") {
        shootPerms_.push_back(ParsePermission(v, ++shootSeq_));
        ShootPushPermissions();
    } else if (cmd == L"permissions_clear") {
        shootPerms_.clear();
        ShootPushPermissions();
    } else if (cmd == L"agents") {
        shootAgents_.sessions.clear();
        const ULONGLONG unixNow = UnixMs();
        for (const Value& s : Items(v, "sessions")) {
            AgentSession a;
            a.tool = Str(s, "tool") == L"codex" ? AgentTool::Codex : AgentTool::Claude;
            a.id = Str(s, "id", L"demo");
            a.project = Str(s, "project");
            a.cwd = Str(s, "cwd");
            a.title = Str(s, "title");
            const std::wstring status = Str(s, "status");
            a.status = status == L"working" ? AgentStatus::Working : status == L"waiting" ? AgentStatus::Waiting : AgentStatus::Idle;
            a.doing = Str(s, "doing");
            a.lastActivity = unixNow;
            a.lastReply = Str(s, "lastReply");
            const auto& feed = Items(s, "feed");
            for (size_t i = 0; i < feed.size(); ++i) {
                AgentEvent e;
                e.kind = EventKind(Str(feed[i], "kind"));
                e.text = Str(feed[i], "text");
                e.detail = Str(feed[i], "detail");
                e.running = Bool(feed[i], "running");
                e.callId = "c" + std::to_string(i);
                e.time = unixNow - static_cast<ULONGLONG>(Num(feed[i], "ago", 0) * 1000);
                a.feed.push_back(std::move(e));
            }
            shootAgents_.sessions.push_back(std::move(a));
        }
        shootAgents_.justStarted = Str(v, "justStarted");
        ++shootAgents_.version;
        for (auto& island : islands_) island->SetAgents(shootAgents_);
    } else if (cmd == L"solu") {                                  // fields present replace the current ones
        SoluSnapshot& s = shootSolu_;
        if (Has(v, "state")) {
            const std::wstring st = Str(v, "state");
            s.state = st == L"listening" ? SoluState::Listening : st == L"thinking" ? SoluState::Thinking
                    : st == L"speaking" ? SoluState::Speaking : st == L"off" ? SoluState::Off : SoluState::Ready;
        }
        if (Has(v, "heard")) s.heard = Str(v, "heard"), ++s.turn;
        if (Has(v, "said")) s.said = Str(v, "said"), ++s.turn;
        if (Has(v, "level")) s.level = static_cast<float>(Num(v, "level"));
        if (Has(v, "mic")) s.mic = static_cast<float>(Num(v, "mic"));
        if (Has(v, "captions")) s.captions = Bool(v, "captions");
        if (Has(v, "voice")) s.voice = Str(v, "voice");
        if (Has(v, "lookTitle")) s.lookTitle = Str(v, "lookTitle");
        if (Has(v, "lookImage")) s.lookImage = LoadPicture(Str(v, "lookImage"), 640);
        if (Has(v, "history")) {
            s.history.clear();
            for (const Value& h : Items(v, "history")) s.history.push_back({Str(h, "heard"), Str(h, "said")});
        }
        if (Has(v, "tasks")) {
            s.tasks.clear();
            for (const Value& t : Items(v, "tasks")) {
                SoluTask task;
                task.id = std::to_string(static_cast<int>(Num(t, "id", 1)));
                task.title = Str(t, "title");
                task.status = Str(t, "status", L"running");
                task.step = Str(t, "step");
                task.question = Str(t, "question");
                task.result = Str(t, "result");
                task.done = static_cast<int>(Num(t, "done"));
                task.total = static_cast<int>(Num(t, "total"));
                s.tasks.push_back(std::move(task));
            }
        }
        if (Has(v, "approvals")) {
            s.approvals.clear();
            for (const Value& a : Items(v, "approvals")) s.approvals.push_back(ParsePermission(a, kSoluApprovalBase + (++shootSeq_)));
            ShootPushPermissions();
        }
        for (auto& island : islands_) {
            island->SetSolu(s);
            if (Bool(v, "show")) island->ShowForSolu();
        }
    } else if (cmd == L"activities") {
        shootActs_.clear();
        for (const Value& a : Items(v, "items")) {
            Activity act;
            act.id = Str(a, "id", L"x");
            const std::wstring kind = Str(a, "kind", L"progress");
            act.kind = kind == L"timer" ? Activity::Kind::Timer : kind == L"nudge" ? Activity::Kind::Nudge
                     : kind == L"done" ? Activity::Kind::Done : Activity::Kind::Progress;
            if (Has(a, "glyph")) act.glyph = Str(a, "glyph");
            act.title = Str(a, "title");
            act.detail = Str(a, "detail");
            act.progress = static_cast<float>(Num(a, "progress", -1));
            act.length = static_cast<uint64_t>(Num(a, "lengthMs"));
            if (Has(a, "endsInMs")) act.endsAt = now + static_cast<uint64_t>(Num(a, "endsInMs"));
            act.accent = static_cast<uint32_t>(Num(a, "accent", 0x0A84FF));
            act.dismissible = Bool(a, "dismissible");
            shootActs_.push_back(std::move(act));
        }
        for (auto& island : islands_) island->SetActivities(shootActs_);
    } else if (cmd == L"system") {
        SystemSnapshot& s = shootSystem_;
        s.valid = true;
        s.stamp = now;
        s.cpuName = Str(v, "cpuName", L"AMD Ryzen 7 5700G");
        s.cpuCores = 8, s.cpuThreads = 16;
        s.cpuUsage = static_cast<float>(Num(v, "cpu"));
        s.cpuClockGHz = static_cast<float>(Num(v, "cpuGHz", 4.2));
        s.cpuHistory.Push(s.cpuUsage);
        s.gpuAvailable = true;
        s.gpuName = Str(v, "gpuName", L"NVIDIA GeForce RTX 5050");
        s.gpuUsage = static_cast<float>(Num(v, "gpu"));
        s.gpuTempC = static_cast<float>(Num(v, "gpuTemp", 52));
        s.gpuFanPercent = static_cast<float>(Num(v, "gpuFan", 34));
        s.vramTotal = 8ull << 30;
        s.vramUsed = static_cast<uint64_t>(Num(v, "vramGB", 2.4) * (1ull << 30));
        s.gpuHistory.Push(s.gpuUsage);
        s.ramTotal = 32ull << 30;
        s.ramUsage = static_cast<float>(Num(v, "ram"));
        s.ramUsed = static_cast<uint64_t>(s.ramTotal * s.ramUsage / 100.0);
        s.ramHistory.Push(s.ramUsage);
        s.downBps = Num(v, "downMbps") * 1e6;
        s.upBps = Num(v, "upMbps") * 1e6;
        s.downHistory.Push(static_cast<float>(s.downBps));
        s.upHistory.Push(static_cast<float>(s.upBps));
        for (auto& island : islands_) island->SetSystem(s);
    } else if (cmd == L"audio") {
        if (const Value* in = Get(v, "input")) shootAudio_.input = ParseSide(*in, 4);
        if (const Value* out = Get(v, "output")) shootAudio_.output = ParseSide(*out, 3);
        if (const Value* t = Get(v, "test")) {
            const std::wstring st = Str(*t, "state");
            shootAudio_.test.state = st == L"running" ? AudioTest::State::Running : st == L"done" ? AudioTest::State::Done : AudioTest::State::Idle;
            shootAudio_.test.input = Bool(*t, "input", true);
            shootAudio_.test.level = static_cast<float>(Num(*t, "level"));
            shootAudio_.test.peak = std::max(shootAudio_.test.peak, shootAudio_.test.level);
            shootAudio_.test.verdict = st == L"done" ? AudioTest::Verdict::Works : AudioTest::Verdict::None;
            if (st == L"done") shootAudio_.test.doneAt = now;
        }
        shootAudio_.valid = true;
        for (auto& island : islands_) island->SetAudio(shootAudio_);
    } else if (cmd == L"camera") {
        shootCamera_.valid = true;
        shootCamera_.devices.clear();
        for (const Value& d : Items(v, "devices")) shootCamera_.devices.push_back({FromUtf8(d.text), true, L""});
        shootCamera_.inUseBy.clear();
        for (const Value& a : Items(v, "inUseBy")) shootCamera_.inUseBy.push_back(FromUtf8(a.text));
        shootCamera_.blocked = Bool(v, "blocked");
        for (auto& island : islands_) island->SetCamera(shootCamera_);
    } else if (cmd == L"shelf") {
        shootShelf_.items.clear();
        for (const Value& f : Items(v, "files")) ShootShelfAdd(FromUtf8(f.text));
        for (auto& island : islands_) island->SetShelf(shootShelf_);
    } else if (cmd == L"media") {                                 // rewind the real player so every take starts alike
        if (media_) {
            if (Has(v, "seek")) media_->Send(MediaCommand::Seek, Num(v, "seek"));
            if (Bool(v, "toggle")) media_->Send(MediaCommand::TogglePlayPause, 0);
            if (Has(v, "playing") && media_->Snapshot().playing != Bool(v, "playing")) media_->Send(MediaCommand::TogglePlayPause, 0);
        }
    } else if (cmd == L"layout") {                                // where the island's buttons are right now
        if (!islands_.empty()) {
            std::ofstream out(Str(v, "file"), std::ios::trunc);
            out << islands_.front()->DebugLayout();
        }
    } else if (cmd == L"tab") {
        for (auto& island : islands_) island->DebugOpenTab(static_cast<int>(Num(v, "section", -1)));
    } else if (cmd == L"cheer") {
        CheerAll();
    } else if (cmd == L"mood") {
        const std::wstring m = Str(v, "mood");
        for (auto& island : islands_) island->SetMood(m == L"sleepy" ? Mascot::Mood::Sleepy : m == L"worried" ? Mascot::Mood::Worried : Mascot::Mood::Normal);
    } else {
        Log("shoot: unknown command %ls", cmd.c_str());
    }
}

void App::ShootPushPermissions() {
    std::vector<PermissionRequest> all = shootPerms_;
    for (const PermissionRequest& r : shootSolu_.approvals) all.push_back(r);
    for (auto& island : islands_) island->SetPermissions(all);
}

void App::ShootShelfAdd(const std::wstring& path) {
    for (const ShelfItem& i : shootShelf_.items) if (i.path == path) return;
    ShelfItem item;
    item.path = path;
    item.name = path.substr(path.find_last_of(L"\\/") + 1);
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        item.isDir = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.size = static_cast<uint64_t>(attr.nFileSizeHigh) << 32 | attr.nFileSizeLow;
    }
    std::wstring ext = item.name.substr(std::min(item.name.size(), item.name.find_last_of(L'.')));
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg") item.icon = LoadPicture(path, 256);
    else item.icon = LoadPicture(path + L".icon.png", 256);       // the director leaves a picture of the file's icon next to it
    shootShelf_.items.push_back(std::move(item));
    ++shootShelf_.version;
}

void App::ShootPushAll() {
    for (auto& island : islands_) {
        island->SetNotifications(shootNotifs_);
        island->SetAgents(shootAgents_);
        island->SetSolu(shootSolu_);
        island->SetActivities(shootActs_);
        island->SetShelf(shootShelf_);
        if (shootSystem_.valid) island->SetSystem(shootSystem_);
        if (shootAudio_.valid) island->SetAudio(shootAudio_);
        if (shootCamera_.valid) island->SetCamera(shootCamera_);
    }
    ShootPushPermissions();
}

// The island's buttons while filming: they change the scripted state only.
void App::ShootDismissNotification(uint32_t id) {
    std::erase_if(shootNotifs_.items, [&](const NotificationItem& n) { return n.id == id; });
    for (auto& island : islands_) island->SetNotifications(shootNotifs_);
}

void App::ShootDecide(uint64_t id) {
    std::erase_if(shootPerms_, [&](const PermissionRequest& r) { return r.id == id; });
    std::erase_if(shootSolu_.approvals, [&](const PermissionRequest& r) { return r.id == id; });
    ShootPushPermissions();
}

void App::ShootSelect(bool input, const std::wstring& id) {
    AudioSide& side = input ? shootAudio_.input : shootAudio_.output;
    for (AudioDevice& d : side.devices) {                         // the island makes the pick the Windows default
        d.isDefault = d.id == id;
        if (d.isDefault) side.name = d.name;
    }
    side.selectedId.clear();
    if (!input) {                                                 // the music follows the picked output
        for (AudioDevice& d : side.devices) {
            std::erase(d.apps, std::wstring(L"Spotify"));
            if (d.name == side.name) d.apps.push_back(L"Spotify");
        }
    }
    for (auto& island : islands_) island->SetAudio(shootAudio_);
}

void App::ShootMute(int side, bool muted) {
    if (side == 0) shootAudio_.input.muted = muted;
    else if (side == 1) shootAudio_.output.muted = muted;
    else if (side == 2) shootCamera_.blocked = muted;
    for (auto& island : islands_) {
        if (side == 2) island->SetCamera(shootCamera_);
        else if (side < 2) island->SetAudio(shootAudio_);
    }
}

} // namespace eflow
#endif
