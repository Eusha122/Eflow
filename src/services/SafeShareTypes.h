#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eflow {

// What the SafeShare tab and the SafeShare service tell each other. Sending by email goes through SafeShare's own server
// (api.safeshare.co): the files are encrypted on this PC, uploaded to a password-protected room that lasts 10 hours, and
// the recipient gets SafeShare's usual email with the link. The phone link is direct, on the same Wi-Fi (no server).

inline constexpr size_t kShareMaxFiles = 100;                              // SafeShare's room limits
inline constexpr uint64_t kShareMaxBytes = 4ull * 1024 * 1024 * 1024;
inline constexpr int kShareRoomHours = 10;
inline constexpr size_t kShareMinPassword = 8, kShareMaxPassword = 128, kShareMaxName = 80, kShareMaxEmail = 254;

// An address that can be emailed: one '@' with something before it, a dotted domain after it, no spaces, not too long.
inline bool IsPlausibleEmail(const std::wstring& s) {
    if (s.size() < 6 || s.size() > kShareMaxEmail) return false;
    const size_t at = s.find(L'@');
    if (at == 0 || at == std::wstring::npos || s.find(L'@', at + 1) != std::wstring::npos) return false;
    const size_t dot = s.rfind(L'.');
    if (dot == std::wstring::npos || dot < at + 2 || dot + 3 > s.size()) return false;    // "a@b.co": a name before the dot, 2+ after
    for (wchar_t c : s)
        if (c <= L' ' || c == L',' || c == L';' || c == L'<' || c == L'>' || c == L'"') return false;
    return s.find(L"..") == std::wstring::npos;
}

struct ShareSendRequest {
    std::vector<std::wstring> paths;           // files (not folders)
    std::wstring sender;                       // "your name", shown in the email
    std::wstring email;                        // the recipient
    std::wstring password;                     // needed to open the room; sent to them another way, never by Eflow
    int maxDevices = 1;                        // how many devices may ever open the room (a forwarded link gets "Room is full")
};

enum class SharePhase { Idle, Preparing, Uploading, Inviting, Sent, Failed, Cancelled };

struct ShareTransfer {                         // the send in progress (one at a time)
    SharePhase phase = SharePhase::Idle;
    std::wstring recipient;
    std::wstring current;                      // Uploading: the file being uploaded
    uint64_t done = 0, total = 0;              // bytes of the files uploaded so far / in all
    double bytesPerSecond = 0;
    int waiting = 0;                           // seconds until SafeShare's rate limit lets the upload go on (0: not waiting)
    std::wstring error;                        // Failed: what went wrong, in plain words
    std::wstring roomId;                       // Sent: the room (Copy link)
};

struct SentShare {                             // a room this PC sent, until it expires
    std::wstring roomId;
    std::wstring title;                        // "report.pdf + 2 more"
    std::wstring recipient;
    int64_t sentAt = 0, expiresAt = 0;         // seconds since 1970
    int opens = 0, downloads = 0;              // devices that opened it, downloads
    std::wstring lastSeen;                     // where it was last opened: "Dhaka, Bangladesh · Chrome on Android"
    bool emailed = true;                       // false: the room is ready but the email could not be sent (copy the link)
};

enum class PhoneState { Off, Starting, Waiting, Asking, Connected, Unavailable };

struct PhoneLink {                             // phone <-> PC on the same Wi-Fi
    PhoneState state = PhoneState::Off;
    std::wstring url;                          // what the QR code holds
    std::vector<uint8_t> qr;                   // its modules, row by row (1 = dark), qrSize x qrSize
    int qrSize = 0;
    std::wstring device;                       // Asking / Connected: the phone ("Android phone, 192.168.0.12")
    std::wstring reason;                       // Unavailable: why ("Public network", "No Wi-Fi")
    bool publicNetwork = false;                // Unavailable because Windows calls the network public (it may be the user's own)
    uint32_t received = 0;                     // files the phone sent this session (they are on the Shelf)
};

struct SafeShareSnapshot {
    ShareTransfer transfer;
    std::vector<SentShare> sent;               // newest first
    PhoneLink phone;
    std::wstring sender;                       // the name used last time (the box starts with it)
    int maxDevices = 1;                        // the choice used last time
    uint32_t version = 0;
};

} // namespace eflow
