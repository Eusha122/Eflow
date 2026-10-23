#include "services/ShareActivity.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "core/Json.h"
#include "core/Utf8.h"

namespace eflow {
namespace {

using json::Value;

std::string Field(Value& v, const char* key) {
    const Value* m = v.Find(key);
    return m && (m->type == Value::Type::String || m->type == Value::Type::Number) ? m->text : std::string();
}

std::wstring Where(Value& log) {
    std::wstring place = FromUtf8(Field(log, "city"));
    const std::wstring country = FromUtf8(Field(log, "country"));
    if (!country.empty()) place += (place.empty() ? L"" : L", ") + country;
    std::wstring device = FromUtf8(Field(log, "browser"));
    const std::wstring os = FromUtf8(Field(log, "os"));
    if (!os.empty()) device += (device.empty() ? L"" : L" on ") + os;
    if (place.empty()) return device;
    return device.empty() ? place : place + L" · " + device;
}

} // namespace

int64_t ParseIsoUtc(const std::string& s) {
    std::tm tm{};
    if (s.size() < 19 || sscanf_s(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    int offset = 0;                                             // seconds east of UTC
    const size_t zone = s.find_first_of("+-Z", 19);
    if (zone != std::string::npos && s[zone] != 'Z') {
        int hours = 0, minutes = 0;
        sscanf_s(s.c_str() + zone + 1, "%2d:%2d", &hours, &minutes);
        offset = (s[zone] == '-' ? -1 : 1) * (hours * 3600 + minutes * 60);
    }
    const int64_t t = static_cast<int64_t>(_mkgmtime(&tm));
    return t < 0 ? 0 : t - offset;
}

std::string FormatIsoUtc(int64_t unix) {
    std::tm tm{};
    const std::time_t t = static_cast<std::time_t>(unix);
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return buf;
}

bool ApplyAccessLog(const std::string& body, int64_t now, int64_t freshSeconds, SentShare& room, std::vector<std::string>& seen,
                    std::vector<ShareNews>& news) {
    Value answer;
    std::string error;
    if (!json::Parse(body, answer, error) || answer.type != Value::Type::Object) return false;
    Value* logs = answer.Find("logs");
    if (!logs || logs->type != Value::Type::Array) return false;
    for (auto it = logs->items.rbegin(); it != logs->items.rend(); ++it) {   // oldest first (the server sends newest first)
        Value& log = *it;
        const std::string id = Field(log, "id"), type = Field(log, "event_type");
        if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        const bool opened = type == "room_access", downloaded = type == "file_download" || type == "bulk_download";
        if (!opened && !downloaded) continue;                    // (leaving, presence)
        ++(opened ? room.opens : room.downloads);
        room.lastSeen = Where(log);
        const int64_t at = ParseIsoUtc(Field(log, "created_at"));
        if (at > 0 && now - at <= freshSeconds) news.push_back({opened, room.lastSeen});
    }
    return true;
}

} // namespace eflow
