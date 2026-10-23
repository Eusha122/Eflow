#pragma once
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "services/PhoneSessions.h"
#include "services/SafeShareTypes.h"

namespace eflow {

// Phone <-> PC on the same Wi-Fi, with no app on the phone and no server anywhere: while it runs, Eflow serves one small
// page on this PC's address in the local network. The QR code holds its link, which carries a random 128-bit token; the
// first phone that opens it must be allowed on the PC, and from then on only that phone (its session cookie) gets in.
// The phone downloads the files offered on the PC and sends files to it (they are saved in Downloads\From phone).
//
// It only starts on a network Windows treats as private (home or work), binds to that network's address alone, and stops
// on Stop, or by itself after 10 minutes without a request. Transfers are plain HTTP inside the local network: the link's
// token and the approval keep others out, but the bytes are not encrypted on the way (browsers allow page encryption only
// on https, which a local page cannot have without a certificate warning).
//
// Network work happens on its own threads; `changed` is posted when Snapshot() has something new, `received` (lParam: a
// heap std::wstring path, the receiver deletes it) for each file the phone sent.
class PhoneShare {
public:
    PhoneShare(HWND notify, UINT changed, UINT received);
    ~PhoneShare();
    PhoneShare(const PhoneShare&) = delete;
    PhoneShare& operator=(const PhoneShare&) = delete;

    void Start(bool evenIfPublic = false);           // evenIfPublic: the user said this "public" network is their own
    void Stop();
    void Allow(bool allow);                              // the phone that asked: let it in, or turn it away
    void Offer(std::vector<std::wstring> paths);         // the files the phone may download
    PhoneLink Snapshot() const;

private:
    struct Request {
        std::string method, path, query, cookie, userAgent;
        uint64_t contentLength = 0;
        bool hasLength = false;
        std::string body;                                // what came with the headers (the rest is read on demand)
    };
    struct Client {
        std::thread thread;
        SOCKET socket = INVALID_SOCKET;
        std::atomic<bool> done{false};
    };

    void Serve(bool evenIfPublic);                       // the accept loop (also ends the link when idle)
    void Handle(SOCKET s, const std::string& ip);
    void Route(SOCKET s, Request& r, const std::string& ip);
    void SendPage(SOCKET s, const std::string& session, bool newSession);
    void SendState(SOCKET s, const std::string& session, const std::string& ip);
    void SendFile(SOCKET s, const std::string& session, const std::string& ip, const std::string& id);
    void ReceiveFile(SOCKET s, Request& r, const std::string& session, const std::string& ip);
    void Shutdown();                                     // stop serving, join the threads (no lock held)
    void Changed();
    void SetState(PhoneState state, const std::wstring& reason = {});

    HWND notify_;
    UINT changedMessage_, receivedMessage_;
    mutable std::mutex mutex_;
    PhoneLink link_;
    std::string prefix_;                                 // "/s/<token>"
    std::string pcName_;
    PhoneSessions sessions_;                             // who asked, who was let in, who was turned away
    std::wstring askingDevice_;
    std::vector<std::wstring> offered_;
    uint32_t offerVersion_ = 0;
    std::atomic<ULONGLONG> lastUse_{0};
    std::atomic<bool> stop_{false};
    SOCKET listener_ = INVALID_SOCKET;
    std::thread server_;
    std::mutex clientsLock_;
    std::list<Client> clients_;
    std::mutex controlLock_;                             // Start / Stop, one at a time
    mutable std::atomic<bool> posted_{false};
    bool wsa_ = false;
};

} // namespace eflow
