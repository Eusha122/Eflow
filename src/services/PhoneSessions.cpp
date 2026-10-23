#include "services/PhoneSessions.h"

namespace eflow {

std::string PhoneSessions::Resolve(const std::string& cookie, const std::string& ip) const {
    if (!cookie.empty() && (cookie == allowed_.session || cookie == asking_.session || deniedSessions_.count(cookie))) return cookie;
    if (!allowed_.session.empty() && ip == allowed_.ip) return allowed_.session;
    if (!asking_.session.empty() && ip == asking_.ip) return asking_.session;
    return {};
}

PhoneSessions::Visit PhoneSessions::Arrive(const std::string& cookie, const std::string& ip, bool accepting) {
    Visit v;
    v.session = Resolve(cookie, ip);
    if (v.session.empty() && deniedIps_.count(ip)) {           // turned away before, now without its cookie
        v.session = newId_();
        deniedSessions_.insert(v.session);
    } else if (v.session.empty()) {
        v.session = newId_();
        if (accepting && asking_.session.empty() && allowed_.session.empty()) {
            asking_ = {v.session, ip};
            v.knocked = true;
        }
    }
    v.newCookie = v.session != cookie;
    return v;
}

PhoneSessions::Status PhoneSessions::StatusOf(const std::string& cookie, const std::string& ip) const {
    const std::string s = Resolve(cookie, ip);
    if (!s.empty() && s == allowed_.session) return Status::Allowed;
    if (!s.empty() && s == asking_.session) return Status::Waiting;
    if (deniedSessions_.count(s) || (s.empty() && deniedIps_.count(ip))) return Status::Denied;
    return Status::Busy;
}

void PhoneSessions::Allow() {
    if (asking_.session.empty()) return;
    allowed_ = asking_;
    asking_ = {};
}

void PhoneSessions::Deny() {
    if (asking_.session.empty()) return;
    deniedSessions_.insert(asking_.session);
    deniedIps_.insert(asking_.ip);
    asking_ = {};
}

void PhoneSessions::Reset() {
    asking_ = allowed_ = {};
    deniedSessions_.clear();
    deniedIps_.clear();
}

} // namespace eflow
