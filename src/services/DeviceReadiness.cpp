#include "services/DeviceReadiness.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <vector>

namespace eflow {
namespace {

struct Bridge {
    const wchar_t* match;          // lower-case fragment of the device name
    const wchar_t* exe;            // the PC app that has to be running
    const wchar_t* label;          // how to call it
    bool phone;                    // true: a phone streams to the PC app, so it needs a live connection
};

const Bridge kBridges[] = {
    {L"iriun", L"IriunWebcam.exe", L"Iriun Webcam", true},
    {L"droidcam", L"DroidCamApp.exe", L"DroidCam", true},
    {L"epoccam", L"EpocCamService.exe", L"EpocCam", true},
    {L"camo", L"Camo Studio.exe", L"Camo", true},
    {L"obs virtual", L"obs64.exe", L"OBS", false},
};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::vector<DWORD> ProcessIds(const wchar_t* exe) {
    std::vector<DWORD> ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return ids;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile, exe) == 0) ids.push_back(entry.th32ProcessID);
    CloseHandle(snapshot);
    return ids;
}

// Does any of these processes hold an established TCP connection (to a phone on the network or over USB)?
bool HasLiveConnection(const std::vector<DWORD>& pids) {
    const std::set<DWORD> wanted(pids.begin(), pids.end());
    for (ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
        ULONG size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
        std::vector<char> buffer(size + 4096);
        size = static_cast<ULONG>(buffer.size());
        if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) continue;
        if (family == AF_INET) {
            const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i)
                if (table->table[i].dwState == MIB_TCP_STATE_ESTAB && wanted.count(table->table[i].dwOwningPid)) return true;
        } else {
            const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i)
                if (table->table[i].dwState == MIB_TCP_STATE_ESTAB && wanted.count(table->table[i].dwOwningPid)) return true;
        }
    }
    return false;
}

} // namespace

Readiness CheckReadiness(const std::wstring& deviceName) {
    const std::wstring lower = Lower(deviceName);
    for (const Bridge& b : kBridges) {
        if (lower.find(b.match) == std::wstring::npos) continue;
        const std::vector<DWORD> pids = ProcessIds(b.exe);
        if (pids.empty()) return {false, std::wstring(b.label) + L" isn't running"};
        if (b.phone && !HasLiveConnection(pids)) return {false, L"Waiting for your phone"};
        return {};
    }
    return {};
}

} // namespace eflow
