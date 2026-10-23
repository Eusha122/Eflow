#pragma once
#include <string>

namespace eflow {

// Windows Firewall and incoming connections to Eflow (the phone link serves a page on the local network).
enum class FirewallAccess { Unknown, NotAsked, Allowed, Blocked };

// What the firewall does with connections to `exe` on the network the PC is on now. Call on a thread with COM.
FirewallAccess InboundAccess(const std::wstring& exe);

// Listens on the network for a few seconds (on a worker thread). The first time a program does that, Windows asks the
// user whether to allow it, so the question comes when the user expects it, not in the middle of sending a file.
void AskInboundAccess();

} // namespace eflow
