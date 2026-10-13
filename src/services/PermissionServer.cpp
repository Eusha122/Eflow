#include "services/PermissionServer.h"

#include <sddl.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cwctype>

#include "core/Log.h"
#include "core/Messages.h"
#include "core/PermissionPipe.h"

namespace eflow {

namespace json = winrt::Windows::Data::Json;

struct PermissionServer::Slot {
    PermissionRequest request;
    HANDLE decided = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    PermissionDecision decision = PermissionDecision::Defer;
    ~Slot() { CloseHandle(decided); }
};

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring Str(const json::JsonObject& o, const wchar_t* key) {
    if (!o || !o.HasKey(key)) return {};
    const auto v = o.GetNamedValue(key);
    return v.ValueType() == json::JsonValueType::String ? std::wstring(v.GetString()) : std::wstring();
}

std::wstring SourceName(const std::string& id) {
    if (id == "claude-code") return L"Claude Code";
    if (id == "codex") return L"Codex";
    return std::wstring(id.begin(), id.end());
}

// Destructive or remote-code patterns worth a visible warning. A hint only: it never blocks or
// approves anything, the user still decides.
bool LooksRisky(const std::wstring& text) {
    const std::wstring t = Lower(text);
    static const wchar_t* patterns[] = {
        L"rm -rf", L"rm -fr", L"rm -r ", L"rmdir /s", L"rd /s", L"del /s", L"del /f", L"del /q", L"format ",
        L"mkfs", L"dd if=", L"git push --force", L"git push -f", L"git reset --hard", L"git clean -f",
        L"git checkout -- .", L"drop table", L"drop database", L"truncate table", L"shutdown", L"reg delete",
        L"chmod -r 777", L"chown -r", L"invoke-expression", L"| iex", L"|iex", L"| sh", L"| bash", L"|sh", L"|bash",
        L":(){", L"> /dev/sd", L"diskpart", L"cipher /w", L"vssadmin delete", L"set-executionpolicy unrestricted",
    };
    for (const wchar_t* p : patterns)
        if (t.find(p) != std::wstring::npos) return true;
    return t.find(L"remove-item") != std::wstring::npos && t.find(L"-recurse") != std::wstring::npos;
}

PermissionRequest Parse(const std::string& sourceId, const std::string& body) {
    PermissionRequest r;
    r.source = SourceName(sourceId);
    const json::JsonObject root = json::JsonObject::Parse(winrt::to_hstring(body));
    r.tool = Str(root, L"tool_name");
    r.cwd = Str(root, L"cwd");
    if (const auto slash = r.cwd.find_last_of(L"\\/"); slash != std::wstring::npos) {
        r.project = r.cwd.substr(slash + 1);
        if (r.project.empty()) r.project = r.cwd;
    } else {
        r.project = r.cwd;
    }

    json::JsonObject input{nullptr};
    if (root.HasKey(L"tool_input") && root.GetNamedValue(L"tool_input").ValueType() == json::JsonValueType::Object)
        input = root.GetNamedObject(L"tool_input");

    const std::wstring tool = Lower(r.tool);
    if (tool == L"bash" || tool == L"powershell" || tool == L"shell" || tool == L"local_shell" || tool == L"exec_command") {
        r.action = L"run a command";
        r.detail = Str(input, L"command");
    } else if (tool == L"apply_patch") {
        r.action = L"apply a patch";
        // Codex sends the whole patch; show which files it touches.
        const std::wstring patch = Str(input, L"command");
        for (const wchar_t* marker : {L"*** Update File: ", L"*** Add File: ", L"*** Delete File: "}) {
            for (size_t at = patch.find(marker); at != std::wstring::npos; at = patch.find(marker, at + 1)) {
                const size_t start = at + wcslen(marker);
                const size_t end = patch.find(L'\n', start);
                if (!r.detail.empty()) r.detail += L", ";
                r.detail += patch.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            }
        }
        if (r.detail.empty()) r.detail = patch;
        r.risky = patch.find(L"*** Delete File: ") != std::wstring::npos;
    } else if (tool == L"edit" || tool == L"multiedit" || tool == L"notebookedit") {
        r.action = L"edit a file";
        r.detail = Str(input, L"file_path");
        if (r.detail.empty()) r.detail = Str(input, L"notebook_path");
    } else if (tool == L"write") {
        r.action = L"write a file";
        r.detail = Str(input, L"file_path");
    } else if (tool == L"webfetch") {
        r.action = L"fetch a web page";
        r.detail = Str(input, L"url");
    } else {
        r.action = L"use " + (r.tool.empty() ? std::wstring(L"a tool") : r.tool);
        r.detail = input ? std::wstring(input.Stringify()) : std::wstring();
    }
    if (r.detail.empty()) r.detail = Str(input, L"description");
    r.risky = r.risky || LooksRisky(r.detail);
    return r;
}

// Read one message with a deadline (the hook writes its request right after connecting).
bool ReadMessage(HANDLE pipe, HANDLE stop, std::string& out) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    std::string buf(64 * 1024, '\0');
    for (;;) {
        DWORD n = 0;
        ResetEvent(ov.hEvent);
        BOOL r = ReadFile(pipe, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &ov);
        if (!r && GetLastError() != ERROR_IO_PENDING && GetLastError() != ERROR_MORE_DATA) break;
        const HANDLE waits[] = {ov.hEvent, stop};
        if (WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &n, TRUE);
            break;
        }
        if (GetOverlappedResult(pipe, &ov, &n, FALSE)) {
            out.append(buf.data(), n);
            ok = true;
            break;
        }
        if (GetLastError() != ERROR_MORE_DATA) break;
        out.append(buf.data(), n);
        if (out.size() > permission::kMaxRequestBytes) break;
    }
    CloseHandle(ov.hEvent);
    return ok;
}

void WriteMessage(HANDLE pipe, const char* text) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    if (WriteFile(pipe, text, static_cast<DWORD>(strlen(text)), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 2000) == WAIT_OBJECT_0) GetOverlappedResult(pipe, &ov, &n, FALSE);
        else CancelIoEx(pipe, &ov);
    }
    CloseHandle(ov.hEvent);
}

bool ClientGone(HANDLE pipe) {
    DWORD avail = 0;
    return !PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && GetLastError() == ERROR_BROKEN_PIPE;
}

} // namespace

PermissionServer::PermissionServer(HWND notify) : notify_(notify) {
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PermissionServer::~PermissionServer() {
    SetEvent(stop_);
    if (listener_.joinable()) listener_.join();
    for (auto& c : clients_)
        if (c.thread.joinable()) c.thread.join();
    CloseHandle(stop_);
}

bool PermissionServer::Start() {
    listener_ = std::thread([this] { Listen(); });
    return true;
}

std::vector<PermissionRequest> PermissionServer::Pending() const {
    std::lock_guard lock(mutex_);
    std::vector<PermissionRequest> out;
    for (const auto& s : slots_) out.push_back(s->request);
    return out;
}

void PermissionServer::Resolve(uint64_t id, PermissionDecision decision) {
    std::lock_guard lock(mutex_);
    for (auto& s : slots_)
        if (s->request.id == id) {
            s->decision = decision;
            SetEvent(s->decided);
        }
}

void PermissionServer::Listen() {
    // Only the current user may open the pipe (protected DACL), and only from this machine.
    const std::wstring sddl = L"D:P(A;;GA;;;" + permission::CurrentUserSid() + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        Log("permissions: cannot build pipe security descriptor (%lu)", GetLastError());
        return;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const std::wstring name = permission::PipeName();
    bool first = true;
    HANDLE connected = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
        // FIRST_PIPE_INSTANCE on the first one: fail rather than share a name someone else created.
        const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE pipe = CreateNamedPipeW(name.c_str(), openMode,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       PIPE_UNLIMITED_INSTANCES, 4096, 64 * 1024, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log("permissions: cannot create pipe (%lu); AI approvals disabled", GetLastError());
            break;
        }
        if (first) Log("permissions: listening for AI tool requests");
        first = false;

        OVERLAPPED ov{};
        ov.hEvent = connected;
        ResetEvent(connected);
        bool ready = ConnectNamedPipe(pipe, &ov) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!ready && GetLastError() == ERROR_IO_PENDING) {
            const HANDLE waits[] = {connected, stop_};
            DWORD n;
            ready = WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 &&
                    GetOverlappedResult(pipe, &ov, &n, FALSE);
            if (!ready) CancelIoEx(pipe, &ov);
        }
        if (!ready) {
            CloseHandle(pipe);
            continue;
        }
        std::lock_guard lock(mutex_);
        std::erase_if(clients_, [](Client& c) {      // reap finished connections
            if (!c.done->load()) return false;
            c.thread.join();
            return true;
        });
        auto done = std::make_shared<std::atomic<bool>>(false);
        clients_.push_back({std::thread([this, pipe, done] {
                                Serve(pipe);
                                done->store(true);
                            }),
                            done});
    }
    CloseHandle(connected);
    LocalFree(sd);
}

void PermissionServer::Serve(HANDLE pipe) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::string message;
    std::shared_ptr<Slot> slot;
    if (ReadMessage(pipe, stop_, message)) {
        // "EFLOW1\n<source>\n<json>"
        const size_t a = message.find('\n');
        const size_t b = a == std::string::npos ? std::string::npos : message.find('\n', a + 1);
        if (b != std::string::npos && message.compare(0, a, permission::kMagic) == 0) {
            try {
                slot = std::make_shared<Slot>();
                slot->request = Parse(message.substr(a + 1, b - a - 1), message.substr(b + 1));
            } catch (const winrt::hresult_error&) {
                Log("permissions: ignoring a malformed request");
                slot.reset();
            }
        }
    }

    if (slot) {
        {
            std::lock_guard lock(mutex_);
            slot->request.id = nextId_++;
            slots_.push_back(slot);
        }
        Log("permissions: %ls asks to %ls", slot->request.source.c_str(), slot->request.action.c_str());
        PostMessageW(notify_, WM_APP_PERMISSION, 0, 0);

        // Wait for the user, while noticing if the tool gives up (answered elsewhere, cancelled, timed out).
        const HANDLE waits[] = {slot->decided, stop_};
        const ULONGLONG deadline = GetTickCount64() + permission::kDecisionTimeoutMs + 5000;
        bool decided = false;
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 250);
            if (w == WAIT_OBJECT_0) { decided = true; break; }
            if (w == WAIT_OBJECT_0 + 1 || ClientGone(pipe) || GetTickCount64() > deadline) break;
        }
        if (decided) {
            const PermissionDecision d = slot->decision;
            WriteMessage(pipe, d == PermissionDecision::Allow ? "allow" : d == PermissionDecision::Deny ? "deny" : "defer");
            FlushFileBuffers(pipe);
            Log("permissions: %s", d == PermissionDecision::Allow ? "approved" : d == PermissionDecision::Deny ? "rejected"
                                                                                                             : "deferred to the tool");
        }
        {
            std::lock_guard lock(mutex_);
            std::erase(slots_, slot);
        }
        PostMessageW(notify_, WM_APP_PERMISSION, 0, 0);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    winrt::uninit_apartment();
}

} // namespace eflow
