#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "services/SafeShareTypes.h"

namespace eflow {

// Reading a SafeShare room's access log (GET /api/access-logs/<room>, newest first): who opened it ("room_access", once per
// device) and who downloaded from it ("file_download", "bulk_download"). Pure functions, unit tested in tests/share_test.cpp.

struct ShareNews {                               // something that happened to a sent room
    bool opened = false;                         // false: downloaded
    std::wstring where;                          // "Rajshahi, BD · Chrome on Windows 10"
};

// Counts the entries not in `seen` into `room` (opens, downloads, where it was last seen) and adds them to `seen`. Returns
// what happened in the last `freshSeconds` before `now` (older activity, from while Eflow was closed, is counted quietly).
// False when `body` is not an access log.
bool ApplyAccessLog(const std::string& body, int64_t now, int64_t freshSeconds, SentShare& room, std::vector<std::string>& seen,
                    std::vector<ShareNews>& news);

int64_t ParseIsoUtc(const std::string& text);    // "2026-10-20T11:33:57.214152+00:00" -> seconds since 1970 (0: unreadable)
std::string FormatIsoUtc(int64_t unix);          // seconds since 1970 -> "2026-10-20T11:33:57.000Z"

} // namespace eflow
