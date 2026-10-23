#pragma once
#include <string>

namespace eflow {

// Secrets kept on disk (a SafeShare room's owner token and key) are sealed with DPAPI for the signed-in Windows user: another
// account, or the file copied to another PC, cannot open them. `purpose` separates one kind of secret from another.
bool ProtectForUser(const std::string& plain, const char* purpose, std::string& sealed);
bool UnprotectForUser(const std::string& sealed, const char* purpose, std::string& plain);

} // namespace eflow
