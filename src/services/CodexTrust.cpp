#include "services/CodexTrust.h"

#include <windows.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "core/Json.h"
#include "core/Log.h"
#include "core/Utf8.h"

namespace eflow {
namespace {

using json::Value;

constexpr DWORD kTimeoutMs = 20000;

// One line of JSON-RPC (the app-server reads and writes newline-delimited messages).
std::string Line(const Value& v) {
    std::string s = json::Write(v, 0);
    std::erase(s, '\n');                     // only structural: newlines inside strings are escaped
    return s + "\n";
}

class AppServer {
public:
    ~AppServer() {
        if (job_) {
            TerminateJobObject(job_, 0);         // the server and anything it started
            CloseHandle(job_);
        }
        for (HANDLE h : {in_, out_, process_})
            if (h) CloseHandle(h);
    }

    bool Start(const std::wstring& program, std::wstring& error) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE childIn = nullptr, childOut = nullptr;
        if (!CreatePipe(&childIn, &in_, &sa, 0) || !CreatePipe(&out_, &childOut, &sa, 0)) {
            error = L"no pipes";
            return false;
        }
        SetHandleInformation(in_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        const bool script = program.size() > 4 && _wcsicmp(program.c_str() + program.size() - 4, L".cmd") == 0;
        std::wstring cmd = script ? L"cmd.exe /d /s /c \"\"" + program + L"\" app-server\"" : L"\"" + program + L"\" app-server";
        wchar_t home[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);

        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = childIn;
        si.hStdOutput = childOut;
        si.hStdError = nul;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                       home[0] ? home : nullptr, &si, &pi);
        CloseHandle(childIn);
        CloseHandle(childOut);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            error = L"couldn't start Codex (" + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_) {
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            AssignProcessToJobObject(job_, pi.hProcess);
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        return true;
    }

    void Stop() {
        if (job_) TerminateJobObject(job_, 1);
        else if (process_) TerminateProcess(process_, 1);
    }

    bool Notify(const char* method) {
        Value v = Value::Object();
        v.Member("method") = Value::String(method);
        return Send(Line(v));
    }

    bool Call(const char* method, Value params, Value& result, std::wstring& error) {
        const std::string id = std::to_string(++seq_);
        Value v = Value::Object();
        v.Member("id") = Value::Number(id);
        v.Member("method") = Value::String(method);
        v.Member("params") = std::move(params);
        if (!Send(Line(v))) {
            error = L"Codex stopped";
            return false;
        }
        for (std::string line; ReadLine(line);) {
            Value msg;
            std::string parseError;
            if (!json::Parse(line, msg, parseError) || msg.type != Value::Type::Object) continue;
            const Value* got = msg.Find("id");
            if (!got || got->text != id) continue;             // notifications, or answers to something else
            if (Value* e = msg.Find("error")) {
                const Value* text = e->type == Value::Type::Object ? e->Find("message") : nullptr;
                error = FromUtf8(std::string(method) + ": " + (text ? text->text : "error"));
                return false;
            }
            if (Value* r = msg.Find("result")) result = std::move(*r);
            return true;
        }
        error = L"Codex closed the connection";
        return false;
    }

private:
    bool Send(const std::string& text) {
        DWORD done = 0;
        return WriteFile(in_, text.data(), static_cast<DWORD>(text.size()), &done, nullptr) && done == text.size();
    }

    bool ReadLine(std::string& line) {
        for (;;) {
            const size_t end = buffer_.find('\n');
            if (end != std::string::npos) {
                line = buffer_.substr(0, end);
                buffer_.erase(0, end + 1);
                return true;
            }
            char chunk[16384];
            DWORD n = 0;
            if (!ReadFile(out_, chunk, sizeof(chunk), &n, nullptr) || n == 0) return false;
            buffer_.append(chunk, n);
        }
    }

    HANDLE in_ = nullptr, out_ = nullptr, process_ = nullptr, job_ = nullptr;
    std::string buffer_;
    int seq_ = 0;
};

struct Hook {
    std::string key, hash, trust;
};

// Eflow's hooks in Codex's list: {"data": [{"hooks": [{"command", "key", "currentHash", "trustStatus"}...]}...]}.
std::vector<Hook> EflowHooks(Value& list) {
    std::vector<Hook> out;
    Value* data = list.Find("data");
    if (!data || data->type != Value::Type::Array) return out;
    auto text = [](Value& o, const char* k) { const Value* v = o.Find(k); return v && v->type == Value::Type::String ? v->text : std::string(); };
    for (Value& entry : data->items) {
        Value* hooks = entry.type == Value::Type::Object ? entry.Find("hooks") : nullptr;
        if (!hooks || hooks->type != Value::Type::Array) continue;
        for (Value& h : hooks->items)
            if (h.type == Value::Type::Object && text(h, "command").find("eflow-hook") != std::string::npos)
                out.push_back({text(h, "key"), text(h, "currentHash"), text(h, "trustStatus")});
    }
    return out;
}

bool ListHooks(AppServer& server, std::vector<Hook>& hooks, std::wstring& error) {
    Value params = Value::Object();
    params.Member("cwds") = Value::Array();
    Value result;
    if (!server.Call("hooks/list", std::move(params), result, error)) return false;
    hooks = EflowHooks(result);
    return true;
}

} // namespace

CodexTrust TrustEflowHookInCodex(std::wstring program, std::wstring& detail) {
    if (program.empty()) {
        wchar_t found[MAX_PATH];
        for (const wchar_t* name : {L"codex.exe", L"codex.cmd"})
            if (SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr)) {
                program = found;
                break;
            }
    }
    if (program.empty()) {
        detail = L"Codex isn't installed on this PC";
        return CodexTrust::NoCodex;
    }

    AppServer server;
    if (!server.Start(program, detail)) return CodexTrust::Failed;
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([&] {                    // a Codex that never answers must not hold this up
        if (WaitForSingleObject(finished, kTimeoutMs) == WAIT_TIMEOUT) server.Stop();
    });
    auto run = [&]() -> CodexTrust {
        Value init = Value::Object();
        Value& client = init.Member("clientInfo") = Value::Object();
        client.Member("name") = Value::String("eflow");
        client.Member("version") = Value::String("1.0");
        Value ignored;
        if (!server.Call("initialize", std::move(init), ignored, detail) || !server.Notify("initialized")) return CodexTrust::Failed;

        std::vector<Hook> hooks;
        if (!ListHooks(server, hooks, detail)) return CodexTrust::Failed;
        if (hooks.empty()) {
            detail = L"Codex doesn't list Eflow's hook";
            return CodexTrust::NoHook;
        }
        Value state = Value::Object();
        for (const Hook& h : hooks) {
            if (h.trust == "trusted" || h.key.empty() || h.hash.empty()) continue;
            Value record = Value::Object();
            record.Member("trusted_hash") = Value::String(h.hash);
            state.Member(h.key) = std::move(record);
        }
        if (state.members.empty()) return CodexTrust::AlreadyTrusted;

        // The key contains dots (".codex", "hooks.json"), so it goes in as an entry of the hooks.state table rather than
        // as part of a dotted key path.
        Value edit = Value::Object();
        edit.Member("keyPath") = Value::String("hooks.state");
        edit.Member("mergeStrategy") = Value::String("upsert");
        edit.Member("value") = std::move(state);
        Value write = Value::Object();
        write.Member("edits") = Value::Array();
        write.Member("edits").items.push_back(std::move(edit));
        write.Member("reloadUserConfig").type = Value::Type::Bool;
        write.Member("reloadUserConfig").boolean = true;
        if (!server.Call("config/batchWrite", std::move(write), ignored, detail)) return CodexTrust::Failed;

        if (!ListHooks(server, hooks, detail)) return CodexTrust::Failed;
        if (std::any_of(hooks.begin(), hooks.end(), [](const Hook& h) { return h.trust != "trusted"; })) {
            detail = L"Codex still doesn't trust the hook";
            return CodexTrust::Failed;
        }
        return CodexTrust::Trusted;
    };
    const CodexTrust result = run();
    SetEvent(finished);
    watchdog.join();
    CloseHandle(finished);
    Log("ai: Codex trust: %d %ls", static_cast<int>(result), detail.c_str());
    return result;
}

} // namespace eflow
