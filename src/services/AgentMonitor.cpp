#include "services/AgentMonitor.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "core/Utf8.h"
#include "core/Json.h"
#include "core/Log.h"
#include "services/HookInstaller.h"

namespace eflow {
namespace {

namespace fs = std::filesystem;
using json::Value;

constexpr size_t kFeedMax = 60;
constexpr uint64_t kFirstRead = 400 * 1024;          // a file seen for the first time: only its tail
constexpr uint64_t kPassMax = 4 * 1024 * 1024;       // bytes read per file per pass
constexpr size_t kBigLine = 300 * 1024;              // lines this long are tool output or file contents: not parsed
constexpr ULONGLONG kRecentMs = 6ull * 3600 * 1000;  // sessions older than this are not shown
constexpr ULONGLONG kStaleMs = 4ull * 60 * 1000;     // "working" with no news for this long: probably stopped
constexpr ULONGLONG kStaleToolMs = 20ull * 60 * 1000;

Value* Get(const Value* v, const char* key) {
    if (!v || v->type != Value::Type::Object) return nullptr;
    return const_cast<Value*>(v)->Find(key);
}

std::string Str(const Value* v, const char* key) {
    const Value* f = Get(v, key);
    return f && f->type == Value::Type::String ? f->text : std::string();
}

bool Flag(const Value* v, const char* key) {
    const Value* f = Get(v, key);
    return f && f->type == Value::Type::Bool && f->boolean;
}

ULONGLONG NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const ULONGLONG t = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000;
}

ULONGLONG FileTimeMs(const FILETIME& ft) {
    const ULONGLONG t = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t > 116444736000000000ull ? (t - 116444736000000000ull) / 10000 : 0;
}

// "2026-10-13T15:24:24.168Z" -> unix milliseconds (0 if it does not parse).
ULONGLONG ParseTime(const std::string& iso) {
    int y, mo, d, h, mi, s;
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    SYSTEMTIME st{static_cast<WORD>(y), static_cast<WORD>(mo), 0, static_cast<WORD>(d), static_cast<WORD>(h), static_cast<WORD>(mi),
                  static_cast<WORD>(s), 0};
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    ULONGLONG ms = FileTimeMs(ft);
    const size_t dot = iso.find('.');
    if (dot != std::string::npos) ms += static_cast<ULONGLONG>(atoi(iso.c_str() + dot + 1)) % 1000;
    return ms;
}

std::wstring Trim(std::wstring s, size_t max) {
    for (wchar_t& c : s)
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    size_t a = 0, b = s.size();
    while (a < b && s[a] == L' ') ++a;
    while (b > a && s[b - 1] == L' ') --b;
    s = s.substr(a, b - a);
    // collapse runs of spaces
    std::wstring out;
    for (wchar_t c : s)
        if (!(c == L' ' && !out.empty() && out.back() == L' ')) out += c;
    if (out.size() > max) out = out.substr(0, max - 1) + L"…";
    return out;
}

std::wstring Base(const std::string& path) {
    const size_t cut = path.find_last_of("\\/");
    return FromUtf8(cut == std::string::npos ? path : path.substr(cut + 1));
}

std::wstring ProjectOf(const std::wstring& cwd) {
    std::wstring p = cwd;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t cut = p.find_last_of(L"\\/");
    std::wstring name = cut == std::wstring::npos ? p : p.substr(cut + 1);
    return name.empty() ? p : name;
}

// A Claude tool call, as a short verb and subject: Bash {command} -> ("Run", "npm test").
void DescribeClaudeTool(const std::string& name, const Value* input, std::wstring& verb, std::wstring& subject) {
    auto field = [&](const char* k) { return Str(input, k); };
    verb = FromUtf8(name);
    if (name == "Bash" || name == "PowerShell") {
        verb = L"Run";
        std::string c = field("command");
        subject = Trim(FromUtf8(c), 90);
    } else if (name == "Read") { verb = L"Read"; subject = Base(field("file_path"));
    } else if (name == "Edit" || name == "MultiEdit" || name == "NotebookEdit") { verb = L"Edit"; subject = Base(field("file_path"));
    } else if (name == "Write") { verb = L"Write"; subject = Base(field("file_path"));
    } else if (name == "Grep" || name == "Glob") { verb = L"Search"; subject = Trim(FromUtf8(field("pattern")), 70);
    } else if (name == "WebSearch") { verb = L"Search the web"; subject = Trim(FromUtf8(field("query")), 70);
    } else if (name == "WebFetch") { verb = L"Open page"; subject = Trim(FromUtf8(field("url")), 70);
    } else if (name == "Task" || name == "Agent") { verb = L"Sub-agent"; subject = Trim(FromUtf8(field("description")), 70);
    } else if (name == "TodoWrite") { verb = L"Update plan"; subject.clear();
    } else if (name.rfind("mcp__", 0) == 0) {
        const size_t last = name.rfind("__");
        verb = FromUtf8(name.substr(last + 2));
        std::replace(verb.begin(), verb.end(), L'_', L' ');
        subject = FromUtf8(name.substr(5, name.find("__", 5) - 5));
    } else {
        for (const char* k : {"description", "command", "file_path", "pattern", "query", "prompt"})
            if (!field(k).empty()) { subject = Trim(FromUtf8(field(k)), 70); break; }
    }
}

// The text of a message "content" that is either a string or a list of {type:"text", text} blocks.
std::string PlainText(const Value* content) {
    if (!content) return {};
    if (content->type == Value::Type::String) return content->text;
    std::string out;
    if (content->type == Value::Type::Array)
        for (const Value& b : content->items)
            if (Str(&b, "type") == "text") out += Str(&b, "text") + "\n";
    return out;
}

bool IsNoise(const std::string& text) {
    // IDE and harness messages that are not something the user wrote.
    return text.empty() || text.rfind("<", 0) == 0 || text.rfind("Caveat:", 0) == 0 || text.rfind("[Image", 0) == 0 ||
           text.find("<system-reminder>") != std::string::npos;
}

std::string JsString(const std::string& js, size_t from) {   // the JSON string literal that starts at js[from] == '"'
    std::string out;
    for (size_t i = from + 1; i < js.size(); ++i) {
        if (js[i] == '\\' && i + 1 < js.size()) {
            const char n = js[++i];
            out += n == 'n' ? ' ' : n == 't' ? ' ' : n;
        } else if (js[i] == '"') {
            break;
        } else {
            out += js[i];
        }
    }
    return out;
}

// A session's folder is where it started: when its shell wanders into a sub-folder, the parent (the shorter path) still wins.
void NoteCwd(AgentSession& s, const std::wstring& cwd) {
    if (cwd.empty()) return;
    auto norm = [](std::wstring p) {
        while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
        for (wchar_t& c : p) c = c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c));
        return p;
    };
    const std::wstring a = norm(cwd), b = norm(s.cwd);
    if (s.cwd.empty() || (a.size() < b.size() && b.compare(0, a.size(), a) == 0 && b[a.size()] == L'\\')) {
        s.cwd = cwd;
        s.project = ProjectOf(cwd);
    }
}

std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') { out.append(backslashes * 2 + 1, L'\\'); out += L'"'; backslashes = 0; continue; }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

std::wstring AppData() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    dir += L"\\Eflow";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring UserProfile() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir;
}

} // namespace

AgentMonitor::AgentMonitor(HWND notify, UINT message) : notify_(notify), message_(message) {
    const std::wstring home = UserProfile();
    claudeRoot_ = home + L"\\.claude\\projects";
    codexRoot_ = home + L"\\.codex\\sessions";
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AgentMonitor::~AgentMonitor() {
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    for (std::thread& t : runners_)
        if (t.joinable()) t.join();                    // they notice the stop event within half a second
    if (stop_) CloseHandle(stop_);
}

void AgentMonitor::Start() {
    thread_ = std::thread([this] { Loop(); });
}

void AgentMonitor::Changed() {
    ++version_;
    if (!posted_.exchange(true)) PostMessageW(notify_, message_, 0, 0);
}

// ---- watching ---------------------------------------------------------------------------------------------------

void AgentMonitor::Loop() {
    HANDLE changes[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    const std::wstring roots[2] = {claudeRoot_, codexRoot_};
    ULONGLONG lastAttempt = 0;
    Rescan();
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        for (int i = 0; i < 2; ++i) {                  // a root that does not exist yet (tool not used so far) is retried
            if (changes[i] == INVALID_HANDLE_VALUE && now - lastAttempt > 15000 && fs::exists(roots[i]))
                changes[i] = FindFirstChangeNotificationW(roots[i].c_str(), TRUE,
                                                          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        }
        if (now - lastAttempt > 15000) lastAttempt = now;
        HANDLE waits[3] = {stop_, nullptr, nullptr};
        DWORD count = 1;
        int which[2] = {-1, -1};
        for (int i = 0; i < 2; ++i)
            if (changes[i] != INVALID_HANDLE_VALUE) { which[count - 1] = i; waits[count++] = changes[i]; }
        const DWORD r = WaitForMultipleObjects(count, waits, FALSE, 4000);   // also every 4 s: statuses age (stale "working")
        if (r == WAIT_OBJECT_0) break;
        if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count) {
            Sleep(250);                                // an agent writes in bursts: one pass for the burst
            FindNextChangeNotification(waits[r - WAIT_OBJECT_0]);
        }
        Rescan();
    }
    for (HANDLE h : changes)
        if (h != INVALID_HANDLE_VALUE) FindCloseChangeNotification(h);
}

void AgentMonitor::Rescan() {
    const ULONGLONG now = NowMs();
    std::vector<std::pair<AgentTool, fs::path>> files;
    std::error_code ec;
    auto recent = [&](const fs::directory_entry& e) {
        FILETIME ft;
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(e.path().c_str(), GetFileExInfoStandard, &data)) return false;
        ft = data.ftLastWriteTime;
        return now - FileTimeMs(ft) < kRecentMs;
    };
    if (fs::exists(claudeRoot_, ec))
        for (const auto& dir : fs::directory_iterator(claudeRoot_, ec)) {
            if (!dir.is_directory(ec)) continue;
            for (const auto& f : fs::directory_iterator(dir.path(), ec))        // top level only: sub-agents live deeper
                if (f.is_regular_file(ec) && f.path().extension() == L".jsonl" && recent(f)) files.emplace_back(AgentTool::Claude, f.path());
        }
    if (fs::exists(codexRoot_, ec)) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        for (int back = 0; back < 3; ++back) {                                  // today, yesterday, the day before
            ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
            u.QuadPart -= static_cast<ULONGLONG>(back) * 864000000000ull;
            FILETIME f2{u.LowPart, u.HighPart};
            SYSTEMTIME s2;
            FileTimeToSystemTime(&f2, &s2);
            wchar_t sub[48];
            swprintf_s(sub, L"\\%04d\\%02d\\%02d", s2.wYear, s2.wMonth, s2.wDay);
            const fs::path dir = fs::path(codexRoot_ + sub);
            if (!fs::exists(dir, ec)) continue;
            for (const auto& f : fs::directory_iterator(dir, ec))
                if (f.is_regular_file(ec) && f.path().extension() == L".jsonl" && recent(f)) files.emplace_back(AgentTool::Codex, f.path());
        }
    }

    bool changed = false;
    bool sending;
    AgentTool sendingTool;
    ULONGLONG sendingSince;
    {                                                   // (the UI thread changes these; everything else here is ours alone)
        std::lock_guard lock(mutex_);
        sending = sending_;
        sendingTool = sendingTool_;
        sendingSince = sendingSince_;
    }
    std::wstring started;
    for (const auto& [tool, path] : files) {
        auto it = tracked_.find(path.wstring());
        if (it == tracked_.end()) {
            Tracked t;
            t.tool = tool;
            t.path = path.wstring();
            t.session.tool = tool;
            std::wstring stem = path.stem().wstring();
            t.session.id = tool == AgentTool::Codex && stem.size() > 36 ? stem.substr(stem.size() - 36) : stem;
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) && sending && sendingTool == tool &&
                FileTimeMs(data.ftCreationTime) + 4000 >= sendingSince)
                t.created = true;                                               // the file a prompt from the island just made
            it = tracked_.emplace(path.wstring(), std::move(t)).first;
        }
        Tracked& t = it->second;
        HANDLE h = CreateFileW(t.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        const uint64_t total = static_cast<uint64_t>(size.QuadPart);
        bool first = t.offset == 0 && t.session.feed.empty() && !t.session.lastActivity;
        if (total < t.offset) t.offset = 0, first = true, t.session.feed.clear();       // the file was replaced
        if (first && total > kFirstRead) t.offset = total - kFirstRead;
        if (total > t.offset) {
            const uint64_t want = std::min<uint64_t>(total - t.offset, kPassMax);
            std::string buf(static_cast<size_t>(want), '\0');
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(t.offset);
            SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
            DWORD got = 0;
            ReadFile(h, buf.data(), static_cast<DWORD>(want), &got, nullptr);
            buf.resize(got);
            size_t end = buf.rfind('\n');
            if (end == std::string::npos) end = got >= want && total - t.offset > kPassMax ? buf.size() - 1 : std::string::npos;   // one giant line
            if (end != std::string::npos) {
                size_t from = 0;
                if (first && t.offset > 0) {                                     // started mid-file: drop the partial first line
                    const size_t nl = buf.find('\n');
                    from = nl == std::string::npos ? end + 1 : nl + 1;
                }
                while (from <= end) {
                    size_t nl = buf.find('\n', from);
                    if (nl == std::string::npos || nl > end) nl = end + 1;
                    if (nl > from) Ingest(t, buf.substr(from, nl - from));
                    from = nl + 1;
                }
                t.offset += end + 1;
                changed = true;
            }
        }
        CloseHandle(h);
        Refresh(t, now);
    }
    // Forget sessions whose file is gone or too old.
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        const bool listed = std::any_of(files.begin(), files.end(), [&](const auto& f) { return f.second.wstring() == it->first; });
        if (!listed) { it = tracked_.erase(it); changed = true; } else { ++it; }
    }
    // A prompt from the island started a new session: tell the UI which one.
    for (auto& [path, t] : tracked_)
        if (t.created && sending && t.session.lastActivity) {
            started = t.session.id;
            t.created = false;
            changed = true;
        }
    bool arrived = sending && now - sendingSince > 90000;                      // gave up waiting for it to show up
    for (auto& [path, t] : tracked_)                                           // anything newer than the prompt means it arrived
        if (sending && t.tool == sendingTool && t.session.lastActivity > sendingSince) arrived = true;
    if (!claudeCheck_ || now - claudeCheck_ > 30000) {                         // is each program installed (not on every pass)
        claudeCheck_ = now;
        const bool c = !FindProgram(AgentTool::Claude).empty(), x = !FindProgram(AgentTool::Codex).empty();
        if (c != claudeFound_ || x != codexFound_) changed = true;
        claudeFound_ = c;
        codexFound_ = x;
    }
    std::vector<AgentSession> list;
    for (const auto& [path, t] : tracked_)
        if (t.session.lastActivity) list.push_back(t.session);
    std::lock_guard lock(mutex_);
    published_ = std::move(list);
    if (!started.empty()) justStarted_ = started;
    if (arrived && sending_) { sending_ = false; changed = true; }
    if (changed) Changed();
}

// ---- parsing ---------------------------------------------------------------------------------------------------

void AgentMonitor::Ingest(Tracked& t, const std::string& line) {
    if (line.size() > kBigLine) {                     // tool output / file contents: only its meaning matters
        if (t.tool == AgentTool::Claude && line.find("\"tool_result\"") != std::string::npos) {
            for (AgentEvent& e : t.session.feed) e.running = false;          // some call finished (which one is not worth parsing for)
            t.turnDone = false;
        }
        t.session.lastActivity = NowMs();
        return;
    }
    if (t.tool == AgentTool::Claude) IngestClaude(t, line);
    else IngestCodex(t, line);
}

void AgentMonitor::IngestClaude(Tracked& t, const std::string& line) {
    Value root;
    std::string err;
    if (!json::Parse(line, root, err) || root.type != Value::Type::Object) return;
    AgentSession& s = t.session;
    const std::string type = Str(&root, "type");
    ULONGLONG time = ParseTime(Str(&root, "timestamp"));
    if (!time) time = NowMs();
    if (type == "ai-title") {
        const std::string title = Str(&root, "aiTitle");
        if (!title.empty()) s.title = FromUtf8(title);
        return;
    }
    if (type != "user" && type != "assistant") return;
    NoteCwd(s, FromUtf8(Str(&root, "cwd")));
    if (const std::string id = Str(&root, "sessionId"); !id.empty()) s.id = FromUtf8(id);
    s.lastActivity = std::max(s.lastActivity, time);
    if (Flag(&root, "isSidechain")) {                 // a sub-agent is busy: the main session is working too
        t.turnDone = false;
        return;
    }
    const Value* message = Get(&root, "message");
    const Value* content = Get(message, "content");
    auto push = [&](AgentEvent e) {
        e.time = time;
        s.feed.push_back(std::move(e));
        if (s.feed.size() > kFeedMax) s.feed.erase(s.feed.begin(), s.feed.begin() + (s.feed.size() - kFeedMax));
    };
    if (type == "user") {
        if (Flag(&root, "isMeta")) return;
        if (content && content->type == Value::Type::Array) {
            for (const Value& block : content->items) {
                const std::string bt = Str(&block, "type");
                if (bt == "tool_result") {
                    const std::string id = Str(&block, "tool_use_id");
                    for (AgentEvent& e : s.feed)
                        if (e.callId == id) e.running = false;
                    t.turnDone = false;
                }
            }
        }
        const std::string text = PlainText(content);
        if (!text.empty() && !IsNoise(text)) {
            if (text.rfind("[Request interrupted", 0) == 0) {
                t.turnDone = true;
                push({AgentEvent::Kind::Failed, L"Stopped by you", L"", false, "", 0});
            } else {
                if (s.title.empty()) s.title = Trim(FromUtf8(text), 60);
                push({AgentEvent::Kind::You, Trim(FromUtf8(text), 400), L"", false, "", 0});
                t.turnDone = false;
            }
        }
        return;
    }
    // assistant
    if (!content || content->type != Value::Type::Array) return;
    const std::string stop = Str(message, "stop_reason");
    for (const Value& block : content->items) {
        const std::string bt = Str(&block, "type");
        if (bt == "text") {
            const std::string text = Str(&block, "text");
            if (text.find_first_not_of(" \r\n\t") == std::string::npos) continue;
            push({AgentEvent::Kind::Said, Trim(FromUtf8(text), 1800), L"", false, "", 0});
            s.lastReply = Trim(FromUtf8(text), 1800);
        } else if (bt == "tool_use") {
            AgentEvent e;
            e.kind = AgentEvent::Kind::Tool;
            DescribeClaudeTool(Str(&block, "name"), Get(&block, "input"), e.text, e.detail);
            e.running = true;
            e.callId = Str(&block, "id");
            push(std::move(e));
        }
    }
    t.turnDone = stop == "end_turn" || stop == "stop_sequence" || stop == "refusal";
    if (t.turnDone && (s.feed.empty() || s.feed.back().kind != AgentEvent::Kind::Done)) push({AgentEvent::Kind::Done, L"Finished", L"", false, "", 0});
}

void AgentMonitor::IngestCodex(Tracked& t, const std::string& line) {
    Value root;
    std::string err;
    if (!json::Parse(line, root, err) || root.type != Value::Type::Object) return;
    AgentSession& s = t.session;
    const std::string type = Str(&root, "type");
    ULONGLONG time = ParseTime(Str(&root, "timestamp"));
    if (!time) time = NowMs();
    const Value* p = Get(&root, "payload");
    const std::string pt = Str(p, "type");
    auto push = [&](AgentEvent e) {
        e.time = time;
        s.feed.push_back(std::move(e));
        if (s.feed.size() > kFeedMax) s.feed.erase(s.feed.begin(), s.feed.begin() + (s.feed.size() - kFeedMax));
    };
    if (type == "session_meta" || type == "turn_context") {
        NoteCwd(s, FromUtf8(Str(p, "cwd")));
        if (type == "session_meta")
            if (const std::string id = Str(p, "id"); !id.empty()) s.id = FromUtf8(id);
        return;
    }
    s.lastActivity = std::max(s.lastActivity, time);
    if (type == "event_msg") {
        if (pt == "task_started") {
            t.turnDone = false;
        } else if (pt == "user_message") {
            std::string text = Str(p, "message");
            const size_t mark = text.find("## My request for Codex:");
            if (mark != std::string::npos) text = text.substr(mark + 24);
            if (!IsNoise(text.substr(0, 1)) || text.find_first_not_of(" \r\n\t") != std::string::npos) {
                const std::wstring w = Trim(FromUtf8(text), 400);
                if (!w.empty()) {
                    if (s.title.empty()) s.title = Trim(w, 60);
                    push({AgentEvent::Kind::You, w, L"", false, "", 0});
                }
            }
            t.turnDone = false;
        } else if (pt == "agent_message") {
            const std::wstring w = Trim(FromUtf8(Str(p, "message")), 1800);
            if (!w.empty()) {
                push({AgentEvent::Kind::Said, w, L"", false, "", 0});
                s.lastReply = w;
            }
        } else if (pt == "task_complete") {
            for (AgentEvent& e : s.feed) e.running = false;
            t.turnDone = true;
            const std::wstring last = Trim(FromUtf8(Str(p, "last_agent_message")), 1800);
            if (!last.empty()) s.lastReply = last;
            if (s.feed.empty() || s.feed.back().kind != AgentEvent::Kind::Done) push({AgentEvent::Kind::Done, L"Finished", L"", false, "", 0});
        } else if (pt == "turn_aborted") {
            for (AgentEvent& e : s.feed) e.running = false;
            t.turnDone = true;
            push({AgentEvent::Kind::Failed, L"Stopped", L"", false, "", 0});
        } else if (pt == "error") {
            push({AgentEvent::Kind::Failed, Trim(FromUtf8(Str(p, "message")), 200), L"", false, "", 0});
        } else if (pt == "web_search_end") {
            push({AgentEvent::Kind::Tool, L"Search the web", Trim(FromUtf8(Str(p, "query")), 70), false, "", 0});
        }
        return;
    }
    if (type != "response_item") return;
    if (pt == "custom_tool_call" || pt == "function_call" || pt == "local_shell_call") {
        AgentEvent e;
        e.kind = AgentEvent::Kind::Tool;
        e.running = true;
        e.callId = Str(p, "call_id");
        const std::string name = Str(p, "name");
        std::string input = Str(p, "input");
        if (input.empty()) input = Str(p, "arguments");
        e.text = L"Run";
        if (name == "apply_patch" || input.rfind("*** Begin Patch", 0) == 0 || input.find("apply_patch") != std::string::npos) {
            e.text = L"Edit";
            for (const char* tag : {"*** Update File: ", "*** Add File: ", "*** Delete File: "}) {
                const size_t at = input.find(tag);
                if (at != std::string::npos) {
                    const size_t from = at + strlen(tag), nl = input.find_first_of("\r\n\\", from);
                    e.detail = Base(input.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
                    break;
                }
            }
        } else {
            size_t at = input.find("command\":\"");
            if (at == std::string::npos) at = input.find("command\\\":\\\"");
            std::string cmd;
            if (at != std::string::npos) {
                const size_t q = input.find('"', input.find(':', at) + 1);
                if (q != std::string::npos) cmd = JsString(input, q);
            }
            if (cmd.empty() && !input.empty() && input.rfind("const", 0) != 0) cmd = input;
            if (cmd.empty() && name != "exec") e.text = FromUtf8(name);
            e.detail = Trim(FromUtf8(cmd), 90);
        }
        push(std::move(e));
        t.turnDone = false;
    } else if (pt == "custom_tool_call_output" || pt == "function_call_output" || pt == "local_shell_call_output") {
        const std::string id = Str(p, "call_id");
        for (AgentEvent& e : s.feed)
            if (e.callId == id) e.running = false;
        t.turnDone = false;
    }
}

void AgentMonitor::Refresh(Tracked& t, ULONGLONG now) {
    AgentSession& s = t.session;
    if (s.project.empty()) s.project = L"(unknown project)";
    const AgentEvent* running = nullptr;
    for (auto it = s.feed.rbegin(); it != s.feed.rend(); ++it)
        if (it->kind == AgentEvent::Kind::Tool && it->running) { running = &*it; break; }
    const ULONGLONG idle = now > s.lastActivity ? now - s.lastActivity : 0;
    if (t.turnDone || idle > (running ? kStaleToolMs : kStaleMs)) {
        s.status = AgentStatus::Idle;
        s.doing.clear();
        for (AgentEvent& e : s.feed) e.running = false;
    } else {
        s.status = AgentStatus::Working;
        s.doing = running ? running->text + (running->detail.empty() ? L"" : L" " + running->detail) : L"Thinking…";
    }
}

AgentSnapshot AgentMonitor::Snapshot() {
    AgentSnapshot snap;
    {
        std::lock_guard lock(mutex_);
        posted_ = false;
        snap.sessions = published_;
        snap.version = version_;
        snap.sendError = sendError_;
        snap.sending = sending_;
        snap.sendingTool = sendingTool_;
        snap.justStarted = std::exchange(justStarted_, {});
    }
    std::sort(snap.sessions.begin(), snap.sessions.end(), [](const AgentSession& a, const AgentSession& b) {
        if ((a.status == AgentStatus::Working) != (b.status == AgentStatus::Working)) return a.status == AgentStatus::Working;
        return a.lastActivity > b.lastActivity;
    });
    if (snap.sessions.size() > 12) snap.sessions.resize(12);
    snap.claudeFound = claudeFound_;
    snap.codexFound = codexFound_;
    return snap;
}

// ---- going to the window where a session runs ---------------------------------------------------------------------

namespace {
struct HostWindow {
    HWND hwnd;
    std::wstring title, exe;
    bool ide, terminal;
};

std::wstring ExeOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring name;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(p, 0, buf, &size)) name.assign(buf, size);
        CloseHandle(p);
    }
    const size_t cut = name.find_last_of(L'\\');
    name = cut == std::wstring::npos ? name : name.substr(cut + 1);
    for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
    return name;
}

void BringToFront(HWND hwnd) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return;
    keybd_event(VK_MENU, 0, 0, 0);                                      // a tap of Alt lets us take the foreground
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(hwnd);
    if (GetForegroundWindow() != hwnd) SwitchToThisWindow(hwnd, TRUE);
}
} // namespace

std::wstring AgentMonitor::FocusHost(const AgentSession& session, bool openChat) {
    std::vector<HostWindow> windows;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {                      // front to back: the first match is the most recently used
        if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0 || GetWindow(hwnd, GW_OWNER)) return TRUE;
        wchar_t title[512];
        GetWindowTextW(hwnd, title, 512);
        const std::wstring exe = ExeOf(hwnd);
        const bool ide = exe == L"code.exe" || exe == L"code - insiders.exe" || exe == L"cursor.exe" || exe == L"windsurf.exe" || exe == L"vscodium.exe";
        const bool terminal = exe == L"windowsterminal.exe" || exe == L"powershell.exe" || exe == L"pwsh.exe" || exe == L"cmd.exe" ||
                              exe == L"wezterm-gui.exe" || exe == L"alacritty.exe" || exe == L"conhost.exe";
        if (ide || terminal) reinterpret_cast<std::vector<HostWindow>*>(lp)->push_back({hwnd, title, exe, ide, terminal});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));

    auto lower = [](std::wstring s) { for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c)); return s; };
    const std::wstring project = lower(session.project), tool = session.tool == AgentTool::Claude ? L"claude" : L"codex";
    const HostWindow* best = nullptr;
    int bestScore = 0;
    for (const HostWindow& w : windows) {
        const std::wstring title = lower(w.title);
        int score = 0;
        if (!project.empty() && title.find(project) != std::wstring::npos) score += 10;     // "... - Eflow - Visual Studio Code"
        if (w.ide) score += 3;
        if (w.terminal && title.find(tool) != std::wstring::npos) score += 6;               // a terminal that says claude / codex
        if (w.terminal && score < 6) continue;
        if (score > bestScore) best = &w, bestScore = score;                                // earlier (more recent) wins ties
    }
    std::wstring did;
    if (best && bestScore >= 10 || (best && best->terminal)) {
        BringToFront(best->hwnd);
        did = best->terminal ? L"terminal" : L"window";
    } else if (!session.cwd.empty()) {                                  // not open anywhere: open the project in VS Code
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"code", (L"\"" + session.cwd + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        did = reinterpret_cast<INT_PTR>(r) > 32 ? L"opened the project" : L"";
    } else if (best) {
        BringToFront(best->hwnd);
        did = L"window";
    }
    if (openChat && session.tool == AgentTool::Claude && !session.id.empty() && (did == L"window" || did == L"opened the project")) {
        Sleep(250);                                                     // the window is in front: it is the one that receives the link
        ShellExecuteW(nullptr, L"open", (L"vscode://anthropic.claude-code/open?session=" + session.id).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    Log("agent: go to %ls (%ls): %ls", session.project.c_str(), session.tool == AgentTool::Claude ? L"Claude" : L"Codex", did.empty() ? L"no window found" : did.c_str());
    return did;
}

// ---- sending prompts ----------------------------------------------------------------------------------------------

std::wstring AgentMonitor::FindProgram(AgentTool tool) const {
    std::error_code ec;
    const std::wstring home = UserProfile();
    std::vector<fs::path> hits;
    auto scan = [&](const fs::path& extensions, const wchar_t* prefix, const wchar_t* tail) {
        if (!fs::exists(extensions, ec)) return;
        for (const auto& e : fs::directory_iterator(extensions, ec)) {
            const std::wstring name = e.path().filename().wstring();
            if (name.rfind(prefix, 0) != 0) continue;
            const fs::path exe = e.path() / tail;
            if (fs::exists(exe, ec)) hits.push_back(exe);
        }
    };
    const wchar_t* exeName = tool == AgentTool::Claude ? L"claude.exe" : L"codex.exe";
    for (const wchar_t* ide : {L"\\.vscode\\extensions", L"\\.vscode-insiders\\extensions", L"\\.cursor\\extensions", L"\\.windsurf\\extensions"}) {
        if (tool == AgentTool::Claude) scan(home + ide, L"anthropic.claude-code-", L"resources\\native-binary\\claude.exe");
        else scan(home + ide, L"openai.chatgpt-", L"bin\\windows-x86_64\\codex.exe");
    }
    if (!hits.empty()) {                                              // the newest extension
        std::sort(hits.begin(), hits.end(), [&](const fs::path& a, const fs::path& b) { return fs::last_write_time(a, ec) > fs::last_write_time(b, ec); });
        return hits.front().wstring();
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, exeName, nullptr, MAX_PATH, found, nullptr)) return found;
    if (tool == AgentTool::Claude) {
        const std::wstring local = home + L"\\.local\\bin\\claude.exe";
        if (fs::exists(local, ec)) return local;
    }
    return {};
}

bool AgentMonitor::Send(AgentTool tool, const std::wstring& sessionId, const std::wstring& cwd, const std::wstring& text, bool newSession,
                        std::wstring& error) {
    const std::wstring program = FindProgram(tool);
    if (program.empty()) {
        error = tool == AgentTool::Claude ? L"Claude Code isn't installed on this PC" : L"Codex isn't installed on this PC";
        return false;
    }
    const bool resume = !newSession && !sessionId.empty();
    std::wstring args = QuoteArg(program);
    if (tool == AgentTool::Claude) {
        args += L" -p " + QuoteArg(text) + L" --output-format json";
        if (resume) args += L" --resume " + QuoteArg(sessionId);
        if (!HookInstaller::IsConnected(AiTool::ClaudeCode)) {          // approvals still come to the island, for this run only
            const std::wstring settings = AppData() + L"\\agent-claude-settings.json";
            wchar_t exe[MAX_PATH * 2];
            const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
            Value command = Value::Object();
            command.Member("type") = Value::String("command");
            command.Member("command") = Value::String(HookInstaller::Command(AiTool::ClaudeCode, fs::path(std::wstring(exe, n)).parent_path() / L"eflow-hook.exe"));
            command.Member("timeout") = Value::Number("120");
            Value entry = Value::Object();
            entry.Member("hooks") = Value::Array();
            entry.Member("hooks").items.push_back(std::move(command));
            Value root = Value::Object();
            root.Member("hooks") = Value::Object();
            root.Member("hooks").Member("PermissionRequest") = Value::Array();
            root.Member("hooks").Member("PermissionRequest").items.push_back(std::move(entry));
            std::ofstream out(settings, std::ios::binary | std::ios::trunc);
            out << json::Write(root);
            out.close();
            args += L" --settings " + QuoteArg(settings);
        }
    } else {
        args += L" exec";
        if (resume) args += L" resume";
        args += L" --skip-git-repo-check --json";
        if (resume) args += L" " + QuoteArg(sessionId);
        args += L" " + QuoteArg(text);
    }
    const std::wstring workDir = !cwd.empty() && fs::exists(cwd) ? cwd : UserProfile();

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    static std::atomic<int> runNumber{0};
    const std::wstring outPath = AppData() + L"\\agent-run-" + std::to_wstring(++runNumber % 4) + L".txt";
    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = args;
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
    CloseHandle(in);
    CloseHandle(out);
    if (!ok) {
        error = L"Couldn't start the agent (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    Log("agent: sent a prompt to %s (%s) in %ls", tool == AgentTool::Claude ? "Claude Code" : "Codex", resume ? "resume" : "new session", workDir.c_str());
    {
        std::lock_guard lock(mutex_);
        sending_ = true;
        sendingTool_ = tool;
        sendingSince_ = NowMs();
        sendError_.clear();
        Changed();
    }
    runners_.emplace_back([this, tool, process = pi.hProcess, thread = pi.hThread, outPath] {
        CloseHandle(thread);
        for (int slices = 0; WaitForSingleObject(process, 500) == WAIT_TIMEOUT; ++slices) {
            if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0 || slices > 2 * 60 * 45) {      // Eflow is closing (the prompt carries on)
                CloseHandle(process);
                return;
            }
        }
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        std::string text;
        {
            std::ifstream f(outPath, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        std::wstring problem;
        if (tool == AgentTool::Claude) {                                   // {"is_error":true,"result":"..."} as the last line
            const size_t nl = text.find_last_of('\n', text.size() > 1 ? text.size() - 2 : 0);
            Value v;
            std::string err;
            if (json::Parse(nl == std::string::npos ? text : text.substr(nl + 1), v, err) && Flag(&v, "is_error")) problem = Trim(FromUtf8(Str(&v, "result")), 160);
        } else {
            size_t at = text.rfind("\"error\"");
            if (at == std::string::npos) at = text.rfind("turn.failed");
            if (at != std::string::npos) {
                const size_t m = text.find("\"message\":\"", at);
                if (m != std::string::npos) problem = Trim(FromUtf8(JsString(text, m + 10)), 160);
            }
        }
        if (problem.empty() && code != 0) problem = Trim(FromUtf8(text.substr(text.size() > 200 ? text.size() - 200 : 0)), 160);
        std::lock_guard lock(mutex_);
        sending_ = false;
        if (!problem.empty()) {
            sendError_ = problem;
            Log("agent: the prompt failed: %ls", problem.c_str());
        }
        Changed();
    });
    return true;
}

} // namespace eflow
