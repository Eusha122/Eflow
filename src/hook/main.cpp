// eflow-hook.exe: the `PermissionRequest` hook command for Claude Code and OpenAI Codex.
//
//   eflow-hook.exe --source claude-code     (reads the hook's JSON on stdin)
//
// Forwards the request to the Eflow island and prints the user's decision in the tools' shared
// output format. Fail-safe by design: whenever Eflow can't be reached, can't be verified, the user
// chooses to answer in the tool, or nobody answers in time, it prints nothing and exits 0, so the
// tool falls back to its own permission prompt. It never approves anything on its own.
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>

#include "core/PermissionPipe.h"

namespace {

using namespace eflow::permission;

std::string ReadStdin() {
    _setmode(_fileno(stdin), _O_BINARY);
    std::string data;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0 && data.size() < kMaxRequestBytes) data.append(buf, n);
    return data;
}

std::wstring ProcessImage(DWORD pid) {
    std::wstring path;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(p, 0, buf, &size)) path.assign(buf, size);
        CloseHandle(p);
    }
    return path;
}

// The pipe must be served by eflow.exe sitting next to this hook; anything else could be a
// process pretending to be Eflow in order to approve requests.
bool ServerIsEflow(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
    wchar_t self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    std::wstring expected(self, n);
    expected = expected.substr(0, expected.find_last_of(L"\\/") + 1) + L"eflow.exe";
    return CompareStringOrdinal(ProcessImage(pid).c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Returns "allow", "deny", or "" (no decision).
std::string AskEflow(const std::string& source, const std::string& request) {
    const std::wstring name = PipeName();
    if (!WaitNamedPipeW(name.c_str(), 1500)) return "";            // Eflow isn't running
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return "";
    std::string decision;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) && ServerIsEflow(pipe)) {
        const std::string message = std::string(kMagic) + "\n" + source + "\n" + request;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD done = 0;
        bool ok = WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), nullptr, &ov) ||
                  GetLastError() == ERROR_IO_PENDING;
        ok = ok && GetOverlappedResult(pipe, &ov, &done, TRUE);
        if (ok) {
            char reply[64]{};
            ResetEvent(ov.hEvent);
            if (ReadFile(pipe, reply, sizeof(reply) - 1, nullptr, &ov) || GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, kDecisionTimeoutMs) == WAIT_OBJECT_0 &&
                    GetOverlappedResult(pipe, &ov, &done, FALSE)) {
                    const std::string r(reply, done);
                    if (r == "allow" || r == "deny") decision = r;
                } else {
                    CancelIoEx(pipe, &ov);                          // no answer in time: let the tool ask
                    GetOverlappedResult(pipe, &ov, &done, TRUE);
                }
            }
        }
        CloseHandle(ov.hEvent);
    }
    CloseHandle(pipe);
    return decision;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::string source = "unknown";
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"--source") == 0) {
            source.clear();
            for (const wchar_t* c = argv[i + 1]; *c; ++c)
                if (*c < 128 && (iswalnum(*c) || *c == L'-')) source += static_cast<char>(*c);
        }

    const std::string request = ReadStdin();
    if (request.empty()) return 0;
    const std::string decision = AskEflow(source, request);
    if (decision == "allow") {
        fputs(R"({"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"}}})", stdout);
    } else if (decision == "deny") {
        fputs(R"({"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"deny",)"
              R"("message":"The user rejected this request in Eflow."}}})", stdout);
    }
    fflush(stdout);
    return 0;
}
