#pragma once
#include <functional>
#include <set>
#include <string>

namespace eflow {

// Who is who on the phone link: which visitor asked to connect, which one was let in, which ones were turned away.
// A visitor is known by its session cookie, and also by its address in the local network: a phone often opens the QR
// link twice (the camera's preview, then the browser), and the second visit carries no cookie yet. It is still the same
// phone, so it is the same visitor. Another device on the network has another address and stays out. Pure logic, unit
// tested in tests/share_test.cpp; PhoneShare holds its lock while it uses it.
class PhoneSessions {
public:
    enum class Status { Waiting, Allowed, Denied, Busy };
    struct Visit {
        std::string session;                             // the visitor's session (set it as the cookie when `newCookie`)
        bool newCookie = false;
        bool knocked = false;                            // this visit asks to connect: the PC is to be asked
    };

    explicit PhoneSessions(std::function<std::string()> newId) : newId_(std::move(newId)) {}

    // A page load. `accepting`: no one is connected or asking yet, so a new visitor asks to connect.
    Visit Arrive(const std::string& cookie, const std::string& ip, bool accepting);
    Status StatusOf(const std::string& cookie, const std::string& ip) const;
    bool IsAllowed(const std::string& cookie, const std::string& ip) const { return StatusOf(cookie, ip) == Status::Allowed; }
    bool Asking() const { return !asking_.session.empty(); }
    void Allow();                                        // the visitor that asked is let in
    void Deny();                                         // ... is turned away, for as long as this link lasts
    void Reset();

private:
    struct Visitor {
        std::string session, ip;
    };
    // The known visitor a request comes from: by its cookie, or else by its address. Empty: a stranger.
    std::string Resolve(const std::string& cookie, const std::string& ip) const;

    std::function<std::string()> newId_;
    Visitor asking_, allowed_;
    std::set<std::string> deniedSessions_, deniedIps_;
};

} // namespace eflow
