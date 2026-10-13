#pragma once
#include <windows.h>
#include <sddl.h>

#include <string>

namespace eflow::permission {

// Wire protocol between eflow-hook.exe (client) and Eflow (server), one exchange per connection:
//   request  (message): "EFLOW1\n<source id>\n<the hook's raw stdin JSON>"
//   response (message): "allow" | "deny" | "defer"   ("defer" = let the tool ask in its own UI)
constexpr char kMagic[] = "EFLOW1";
constexpr DWORD kMaxRequestBytes = 1024 * 1024;
constexpr DWORD kDecisionTimeoutMs = 290 * 1000;   // a little under the 300 s hook timeout we install

// String form of the current user's SID, e.g. "S-1-5-21-...".
inline std::wstring CurrentUserSid() {
    std::wstring result;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return result;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buffer(size, '\0');
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
            result = sid;
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return result;
}

// Per-user pipe name, so two signed-in users never talk to each other's island.
inline std::wstring PipeName() { return L"\\\\.\\pipe\\Eflow.Permission." + CurrentUserSid(); }

} // namespace eflow::permission
