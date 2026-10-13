#pragma once
#include <windows.h>

namespace eflow {

// True when this process runs with package identity (needed by UserNotificationListener).
bool HasPackageIdentity();

// A sparse package grants identity only to processes started through the package, not to a plain
// double-click of eflow.exe. If Eflow's package is registered for the folder this exe lives in, starts
// the packaged instance and returns true (the caller should exit). Returns false when there is no such
// package or `alreadyRelaunched` is set, in which case the app just runs without identity.
bool RelaunchWithPackageIdentity(bool alreadyRelaunched);

} // namespace eflow
