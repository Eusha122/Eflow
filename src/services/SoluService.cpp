#include "services/SoluService.h"

#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/Json.h"
#include "core/Log.h"

namespace eflow {

const SoluVoice kSoluVoices[5] = {
    {L"cozy", L"Cozy", L"Warm and gentle, like a purring friend"},
    {L"bubbly", L"Bubbly", L"Bright and excited, quick little voice"},
    {L"tiny", L"Tiny", L"Small and squeaky-sweet, the cutest"},
    {L"calm", L"Calm", L"Soft and unhurried, good for late evenings"},
    {L"natural", L"Natural", L"No pet effect, just the plain voice"},
};

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring AppDataDir() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\Eflow";
        CreateDirectoryW(path.c_str(), nullptr);
    }
    CoTaskMemFree(base);
    return path;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Quote(const std::string& s) {        // a JSON string literal
    json::Value v = json::Value::String(s);
    std::string text = json::Write(v, 0);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

} // namespace

SoluService::SoluService(HWND notify, UINT message) : notify_(notify), message_(message) {}

SoluService::~SoluService() { Kill(); }

void SoluService::Start() {
    LoadSettings();
    bool on;
    {
        std::lock_guard lock(mutex_);
        on = snapshot_.enabled;
    }
    if (on) Enable(true);
}

// ---- settings: {"enabled": true, "voice": "cozy", "headphones": false} -------------------------------------

void SoluService::LoadSettings() {
    const std::wstring path = AppDataDir() + L"\\solu.json";
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    std::stringstream text;
    text << file.rdbuf();
    json::Value root;
    std::string error;
    if (!json::Parse(text.str(), root, error) || root.type != json::Value::Type::Object) return;
    std::lock_guard lock(mutex_);
    if (const json::Value* v = root.Find("enabled"); v && v->type == json::Value::Type::Bool) snapshot_.enabled = v->boolean;
    if (const json::Value* v = root.Find("headphones"); v && v->type == json::Value::Type::Bool) snapshot_.headphones = v->boolean;
    if (const json::Value* v = root.Find("captions"); v && v->type == json::Value::Type::Bool) snapshot_.captions = v->boolean;
    if (const json::Value* v = root.Find("voice"); v && v->type == json::Value::Type::String) {
        const std::wstring id = FromUtf8(v->text);
        for (const SoluVoice& voice : kSoluVoices)
            if (id == voice.id) snapshot_.voice = id;
    }
}

void SoluService::SaveSettings() const {
    json::Value root = json::Value::Object();
    json::Value on, headphones;
    on.type = json::Value::Type::Bool;
    on.boolean = snapshot_.enabled;
    headphones.type = json::Value::Type::Bool;
    headphones.boolean = snapshot_.headphones;
    root.Member("enabled") = on;
    root.Member("voice") = json::Value::String(ToUtf8(snapshot_.voice));
    root.Member("headphones") = headphones;
    json::Value captions;
    captions.type = json::Value::Type::Bool;
    captions.boolean = snapshot_.captions;
    root.Member("captions") = captions;
    const std::wstring path = AppDataDir() + L"\\solu.json";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) file << json::Write(root);
}

// ---- process -------------------------------------------------------------------------------------------------

std::wstring SoluService::FindDir() const {
    std::vector<std::wstring> candidates;
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"EFLOW_SOLU_DIR", env, MAX_PATH)) candidates.push_back(env);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\'));
    candidates.push_back(dir + L"\\solu");               // installed next to eflow.exe
    candidates.push_back(dir + L"\\..\\..\\solu");       // development: the repository's solu folder (from build\debug)
    for (const std::wstring& c : candidates)
        if (Exists(c + L"\\solu_service.py") && Exists(c + L"\\venv\\Scripts\\python.exe")) return c;
    return {};
}

bool SoluService::Launch() {
    const std::wstring dir = FindDir();
    if (dir.empty()) {
        std::lock_guard lock(mutex_);
        snapshot_.state = SoluState::Missing;
        snapshot_.error = L"Solu isn't installed on this PC";
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE childIn = nullptr, parentOut = nullptr, parentIn = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &parentOut, &sa, 0) || !CreatePipe(&parentIn, &childOut, &sa, 0)) return false;
    SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0);       // only the child's ends are inherited
    SetHandleInformation(parentIn, HANDLE_FLAG_INHERIT, 0);
    const std::wstring logPath = AppDataDir() + L"\\solu.log";
    HANDLE log = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childIn;
    si.hStdOutput = childOut;
    si.hStdError = log != INVALID_HANDLE_VALUE ? log : childOut;
    std::wstring voice, headphones;
    {
        std::lock_guard lock(mutex_);
        voice = snapshot_.voice;
        headphones = snapshot_.headphones ? L" --headphones" : L"";
    }
    std::wstring command = L"\"" + dir + L"\\venv\\Scripts\\python.exe\" -u solu_service.py --voice " + voice + headphones;
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                   dir.c_str(), &si, &pi);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        CloseHandle(parentOut);
        CloseHandle(parentIn);
        std::lock_guard lock(mutex_);
        snapshot_.state = SoluState::Off;
        snapshot_.error = L"Solu could not be started";
        Log("solu: CreateProcess failed (%lu)", GetLastError());
        return false;
    }
    // A job object that kills Solu when Eflow goes away (even if Eflow crashes).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard lock(mutex_);
        process_ = pi.hProcess;
        writePipe_ = parentOut;
        job_ = job;
        snapshot_.state = SoluState::Loading;
        snapshot_.error.clear();
    }
    reader_ = std::thread([this, process = pi.hProcess, pipe = parentIn] { ReaderLoop(process, pipe); });
    Log("solu: started (%ls)", dir.c_str());
    return true;
}

void SoluService::Kill() {
    HANDLE process, pipe, job;
    {
        std::lock_guard lock(mutex_);
        process = std::exchange(process_, nullptr);
        pipe = std::exchange(writePipe_, nullptr);
        job = std::exchange(job_, nullptr);
    }
    if (pipe) {
        const char quit[] = "{\"cmd\":\"quit\"}\n";
        DWORD written;
        WriteFile(pipe, quit, sizeof(quit) - 1, &written, nullptr);
        CloseHandle(pipe);
    }
    if (process && WaitForSingleObject(process, 1500) == WAIT_TIMEOUT) TerminateProcess(process, 0);
    if (job) CloseHandle(job);                    // anything still alive in the job dies with it
    if (reader_.joinable()) reader_.join();
    if (process) CloseHandle(process);
}

void SoluService::ReaderLoop(HANDLE process, HANDLE readPipe) {
    std::string buffer;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(readPipe, chunk, sizeof(chunk), &got, nullptr) && got) {
        buffer.append(chunk, got);
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            json::Value root;
            std::string error;
            if (!json::Parse(line, root, error) || root.type != json::Value::Type::Object) continue;
            const json::Value* ev = root.Find("event");
            if (!ev) continue;
            bool coalesce = false, talkNow = false;
            {
                std::lock_guard lock(mutex_);
                if (ev->text == "state") {
                    const json::Value* st = root.Find("state");
                    if (st) {
                        if (st->text == "loading") snapshot_.state = SoluState::Loading;
                        else if (st->text == "ready") snapshot_.state = SoluState::Ready;
                        else if (st->text == "listening") {
                            if (snapshot_.state == SoluState::Ready) snapshot_.heard.clear(), snapshot_.said.clear();   // a new conversation
                            snapshot_.state = SoluState::Listening;
                        }
                        else if (st->text == "thinking") snapshot_.state = SoluState::Thinking;
                        else if (st->text == "speaking") snapshot_.state = SoluState::Speaking;
                        if (snapshot_.state == SoluState::Ready) snapshot_.level = 0;
                        if (snapshot_.state != SoluState::Ready && snapshot_.state != SoluState::Loading) snapshot_.error.clear();
                        if (snapshot_.state == SoluState::Ready && talkWhenReady_) talkNow = true, talkWhenReady_ = false;
                    }
                } else if (ev->text == "heard") {
                    if (const json::Value* t = root.Find("text")) {
                        snapshot_.heard = pendingHeard_ = FromUtf8(t->text);
                        snapshot_.said.clear();
                        ++snapshot_.turn;
                    }
                } else if (ev->text == "said") {
                    if (const json::Value* t = root.Find("text")) {
                        snapshot_.said = FromUtf8(t->text);
                        ++snapshot_.turn;
                        if (!pendingHeard_.empty()) {                    // remember the exchange for the Solu AI tab
                            snapshot_.history.push_back({pendingHeard_, snapshot_.said});
                            if (snapshot_.history.size() > 4) snapshot_.history.erase(snapshot_.history.begin());
                            pendingHeard_.clear();
                        }
                    }
                } else if (ev->text == "mic") {
                    if (const json::Value* v = root.Find("v")) snapshot_.mic = static_cast<float>(atof(v->text.c_str()));
                    coalesce = true;
                } else if (ev->text == "setting") {                       // Solu changed one of its own settings by voice
                    const json::Value* name = root.Find("name");
                    const json::Value* value = root.Find("value");
                    if (name && value) {
                        if (name->text == "captions") snapshot_.captions = value->boolean;
                        else if (name->text == "headphones") snapshot_.headphones = value->boolean;
                        else if (name->text == "voice") snapshot_.voice = FromUtf8(value->text);
                        else if (name->text == "enabled" && !value->boolean) snapshot_.sleepRequested = true;
                        SaveSettings();
                    }
                } else if (ev->text == "level") {
                    if (const json::Value* v = root.Find("v")) snapshot_.level = static_cast<float>(atof(v->text.c_str()));
                    coalesce = true;
                } else if (ev->text == "error") {
                    if (const json::Value* m = root.Find("message")) snapshot_.error = FromUtf8(m->text);
                }
            }
#ifndef NDEBUG
            if (ev->text == "state") Log("solu: state -> %s", root.Find("state") ? root.Find("state")->text.c_str() : "?");
            if (ev->text == "heard" || ev->text == "said") Log("solu: %s: %ls", ev->text.c_str(), FromUtf8(root.Find("text") ? root.Find("text")->text : "").c_str());
            if (ev->text == "error") Log("solu: error: %ls", FromUtf8(root.Find("message") ? root.Find("message")->text : "").c_str());
#endif
            if (talkNow) Send("{\"cmd\":\"talk\"}");
            Changed(coalesce);
        }
    }
    // The pipe closed: Solu is gone (switched off, or it died).
    DWORD exitCode = 0;
    GetExitCodeProcess(process, &exitCode);
    {
        std::lock_guard lock(mutex_);
        if (process_ == process) {                              // not a deliberate Kill(): it ended by itself
            if (exitCode != 0) {
                snapshot_.error = L"Solu stopped unexpectedly";
                snapshot_.enabled = false;                       // do not keep restarting something that keeps failing
                SaveSettings();
            }
            snapshot_.state = SoluState::Off;
            snapshot_.level = 0;
            if (writePipe_) CloseHandle(writePipe_);
            if (job_) CloseHandle(job_);
            writePipe_ = nullptr;
            job_ = nullptr;
            process_ = nullptr;                                  // so that Enable(true) can start it again
            CloseHandle(process);
        }
    }
    CloseHandle(readPipe);
    Changed(false);
}

void SoluService::Send(const std::string& json) {
    std::lock_guard lock(mutex_);
    if (!writePipe_) return;
    const std::string line = json + "\n";
    DWORD written = 0;
    WriteFile(writePipe_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void SoluService::Changed(bool coalesce) {
    if (coalesce && posted_.exchange(true)) return;             // one pending message is enough for a stream of levels
    if (!coalesce) posted_ = true;
    PostMessageW(notify_, message_, 0, 0);
}

SoluSnapshot SoluService::Snapshot() {
    std::lock_guard lock(mutex_);
    posted_ = false;
    SoluSnapshot copy = snapshot_;
    snapshot_.mic = 0;                                          // the helper only reports it while somebody talks
    return copy;
}

// ---- commands ------------------------------------------------------------------------------------------------

void SoluService::Enable(bool on) {
    Log("solu: switched %s", on ? "on" : "off");
    if (on) {
        {
            std::lock_guard lock(mutex_);
            snapshot_.enabled = true;
            snapshot_.error.clear();
            SaveSettings();
            if (process_) return;                                // already running
        }
        if (reader_.joinable()) reader_.join();                  // an earlier helper that has already ended
        Launch();
    } else {
        {
            std::lock_guard lock(mutex_);
            snapshot_.enabled = false;
            snapshot_.sleepRequested = false;
            SaveSettings();
            talkWhenReady_ = false;
        }
        Kill();
        std::lock_guard lock(mutex_);
        snapshot_.state = SoluState::Off;
        snapshot_.level = 0;
    }
    Changed(false);
}

void SoluService::Talk() {
    SoluState state;
    {
        std::lock_guard lock(mutex_);
        state = snapshot_.state;
        if (state == SoluState::Loading) talkWhenReady_ = true;  // it will start listening as soon as it is awake
    }
    if (state == SoluState::Off || state == SoluState::Missing) {
        {
            std::lock_guard lock(mutex_);
            talkWhenReady_ = true;
        }
        Enable(true);
        return;
    }
    if (state != SoluState::Loading) Send("{\"cmd\":\"talk\"}");
}

void SoluService::StopSpeaking() { Send("{\"cmd\":\"stop\"}"); }

void SoluService::SetVoice(const std::wstring& id) {
    {
        std::lock_guard lock(mutex_);
        snapshot_.voice = id;
        SaveSettings();
    }
    Send("{\"cmd\":\"voice\",\"name\":" + Quote(ToUtf8(id)) + "}");
    Changed(false);
}

void SoluService::SetHeadphones(bool on) {
    {
        std::lock_guard lock(mutex_);
        snapshot_.headphones = on;
        SaveSettings();
    }
    Send(std::string("{\"cmd\":\"headphones\",\"on\":") + (on ? "true" : "false") + "}");
    Changed(false);
}

void SoluService::SetCaptions(bool on) {
    {
        std::lock_guard lock(mutex_);
        snapshot_.captions = on;
        SaveSettings();
    }
    Changed(false);
}

void SoluService::Ask(const std::wstring& text) { Send("{\"cmd\":\"ask\",\"text\":" + Quote(ToUtf8(text)) + "}"); }

} // namespace eflow
