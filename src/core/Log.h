#pragma once

namespace eflow {

// Appends a timestamped line to %LOCALAPPDATA%\Eflow\eflow.log (and the debugger output).
// printf-style formatting. Cheap enough for lifecycle events and errors; not for per-frame use.
void Log(const char* fmt, ...);

} // namespace eflow
