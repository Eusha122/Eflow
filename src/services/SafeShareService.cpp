#include "services/SafeShareService.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include "core/Json.h"
#include "core/Log.h"
#include "core/Protect.h"
#include "core/Utf8.h"
#include "services/ShareActivity.h"
#include "services/ShareCrypto.h"

namespace eflow {
namespace {

namespace fs = std::filesystem;
namespace sc = sharecrypto;
using json::Value;
using namespace std::chrono_literals;

constexpr wchar_t kApi[] = L"https://api.safeshare.co";
constexpr char kSite[] = "https://safeshare.co";
constexpr wchar_t kUserAgent[] = L"Eflow (Windows; SafeShare)";
constexpr int kUploadThreads = 3;                                 // parts in flight at once (each holds ~50 MB while it works)
constexpr int kTries = 4;                                         // a request is tried this often before the send fails
constexpr auto kWatchEvery = 30s;                                 // one access-log check per 30 s: ~30 of SafeShare's 300 per 15 min
constexpr auto kLongestWait = 15min;                              // a rate limit longer than this fails the send instead
constexpr int64_t kFreshEvent = 10 * 60;                          // older activity (Eflow was off) is counted without a pill
constexpr size_t kSeenKept = 300;
constexpr const char* kSecretPurpose = "Eflow.SafeShare.room.v1";

struct ShareError {                                               // a send failed; `message` is for the user
    std::wstring message;
};

int64_t UnixNow() { return static_cast<int64_t>(std::time(nullptr)); }

fs::path StorePath() {
    PWSTR base = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) path = fs::path(base) / L"Eflow" / L"safeshare.json";
    CoTaskMemFree(base);
    return path;
}

// ---- JSON --------------------------------------------------------------------------------------------------------------

Value Str(const std::string& s) { return Value::String(s); }
Value Str(const std::wstring& s) { return Value::String(ToUtf8(s)); }
Value Num(int64_t n) { return Value::Number(std::to_string(n)); }
Value Bool(bool b) {
    Value v;
    v.type = Value::Type::Bool;
    v.boolean = b;
    return v;
}

std::string Field(Value& v, const char* key) {
    const Value* m = v.Find(key);
    return m && (m->type == Value::Type::String || m->type == Value::Type::Number) ? m->text : std::string();
}

int64_t IntField(Value& v, const char* key) {
    const std::string text = Field(v, key);
    return text.empty() ? 0 : std::strtoll(text.c_str(), nullptr, 10);
}

bool Parse(const std::string& text, Value& out) {
    std::string error;
    return json::Parse(text, out, error) && out.type == Value::Type::Object;
}

std::string ServerError(const HttpResponse& r) {                  // the "error" an API answer carries, if any
    Value v;
    return Parse(r.body, v) ? Field(v, "error") : std::string();
}

// ---- what to tell the user -----------------------------------------------------------------------------------------------

std::wstring Explain(const HttpResponse& r) {
    if (r.status == 0) {
        switch (r.error) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
        case ERROR_WINHTTP_TIMEOUT: return L"Can't reach SafeShare. Check your internet connection and try again.";
        case ERROR_WINHTTP_SECURE_FAILURE: return L"A secure connection to SafeShare couldn't be made (is the PC's date and time right?).";
        default: return L"The connection to SafeShare broke off. Try again.";
        }
    }
    const std::string said = ServerError(r);
    if (r.status == 413) return said.empty() ? L"That's more than a SafeShare room can hold (100 files, 4 GB)." : FromUtf8(said) + L".";
    if (r.status == 429) return L"SafeShare is limiting uploads from this connection. Try again in a few minutes.";
    if (r.status == 503) return L"SafeShare is very busy right now. Try again in a minute.";
    if (r.status >= 500) return L"SafeShare had a problem (" + std::to_wstring(r.status) + L"). Try again in a moment.";
    return L"SafeShare refused the request" + (said.empty() ? std::wstring(L".") : L": " + FromUtf8(said) + L".");
}

std::string ContentType(const std::wstring& path) {               // as the browser would label it (the website previews by it)
    const std::wstring ext = fs::path(path).extension().wstring();
    wchar_t value[128];
    DWORD size = sizeof(value);
    if (!ext.empty() && RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), L"Content Type", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
        return ToUtf8(value);
    return "application/octet-stream";
}

// A file as it is when the send starts: it must still be there, and readable.
HANDLE OpenForReading(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

bool ReadAt(HANDLE file, uint64_t offset, uint8_t* out, size_t size) {
    size_t done = 0;
    while (done < size) {
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>((offset + done) & 0xFFFFFFFF);
        at.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
        DWORD read = 0;
        if (!ReadFile(file, out + done, static_cast<DWORD>(std::min<size_t>(size - done, 8u * 1024 * 1024)), &read, &at) || read == 0) return false;
        done += read;
    }
    return true;
}

} // namespace

// ---- lifetime ----------------------------------------------------------------------------------------------------------

SafeShareService::SafeShareService(HWND notify, UINT changed, UINT event)
    : notify_(notify), changedMessage_(changed), eventMessage_(event), http_(kUserAgent) {
    Load();
    worker_ = std::thread([this] { Run(); });
}

SafeShareService::~SafeShareService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cancel_ = true;
    http_.Cancel();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SafeShareService::Send(ShareSendRequest request) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SharePhase p = transfer_.phase;
    if (p == SharePhase::Preparing || p == SharePhase::Uploading || p == SharePhase::Inviting) return;
    if (std::any_of(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.kind == Job::Kind::Send; })) return;
    transfer_ = {};
    transfer_.phase = SharePhase::Preparing;                       // shown at once; the worker starts on it next
    transfer_.recipient = request.email;
    cancel_ = false;                                               // (here, so a Cancel pressed before the worker starts holds)
    http_.Reset();
    ++version_;
    Job job;
    job.kind = Job::Kind::Send;
    job.request = std::move(request);
    jobs_.push_back(std::move(job));
    Changed();
    wake_.notify_all();
}

void SafeShareService::Cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const SharePhase p = transfer_.phase;
        if (p != SharePhase::Preparing && p != SharePhase::Uploading && p != SharePhase::Inviting) return;
    }
    cancel_ = true;
    http_.Cancel();
    wake_.notify_all();
}

void SafeShareService::Dismiss() {
    std::lock_guard<std::mutex> lock(mutex_);
    const SharePhase p = transfer_.phase;
    if (p == SharePhase::Sent || p == SharePhase::Failed || p == SharePhase::Cancelled) {
        transfer_ = {};
        ++version_;
        Changed();
    }
}

void SafeShareService::Close(const std::wstring& roomId) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job job;
    job.kind = Job::Kind::Close;
    job.roomId = roomId;
    jobs_.push_back(std::move(job));
    wake_.notify_all();
}

std::wstring SafeShareService::Link(const std::wstring& roomId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Room& r : rooms_)
        if (r.info.roomId == roomId && !r.key.empty()) return FromUtf8(std::string(kSite) + "/room/" + ToUtf8(roomId) + "#key=" + r.key);
    return {};
}

SafeShareSnapshot SafeShareService::Snapshot() const {
    posted_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    SafeShareSnapshot s;
    s.transfer = transfer_;
    for (const Room& r : rooms_)
        if (r.complete) s.sent.push_back(r.info);
    s.sender = sender_;
    s.maxDevices = maxDevices_;
    s.version = version_;
    return s;
}

void SafeShareService::Changed() {
    if (!posted_.exchange(true)) PostMessageW(notify_, changedMessage_, 0, 0);
}

void SafeShareService::Post(Event event) const {
    auto* heap = new Event(std::move(event));
    if (!PostMessageW(notify_, eventMessage_, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
}

void SafeShareService::EditTransfer(const std::function<void(ShareTransfer&)>& edit) {
    std::lock_guard<std::mutex> lock(mutex_);
    edit(transfer_);
    ++version_;
    Changed();
}

bool SafeShareService::Pause(std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, wait, [&] { return stop_ || cancel_.load(); });
    return !stop_ && !cancel_;
}

// ---- the worker ----------------------------------------------------------------------------------------------------------

void SafeShareService::Run() {
    for (;;) {
        std::optional<Job> job;                                    // none: it is time to watch the sent rooms
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [&] { return stop_ || !jobs_.empty(); };
            if (nextWatch_ == Clock::time_point::max()) wake_.wait(lock, ready);
            else wake_.wait_until(lock, nextWatch_, ready);
            if (stop_) return;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            } else if (Clock::now() < nextWatch_) {
                continue;                                          // (woken early, nothing to do yet)
            }
        }
        if (!job) Watch();
        else if (job->kind == Job::Kind::Send) DoSend(job->request);
        else DoClose(job->roomId, job->quiet);
    }
}

// The website's API, with the retries a flaky connection and a busy server call for. `waitOutLimits`: a rate limit is
// waited out (the user sees the countdown) rather than failing the send.
HttpResponse SafeShareService::Api(const wchar_t* method, const std::wstring& path, const std::string& body, const std::string& token,
                                   bool waitOutLimits) {
    HttpRequest request;
    request.method = method;
    request.url = std::wstring(kApi) + path;
    if (!body.empty()) request.headers.emplace_back(L"Content-Type", L"application/json");
    if (!token.empty()) request.headers.emplace_back(L"x-author-token", FromUtf8(token));
    request.body = reinterpret_cast<const uint8_t*>(body.data());
    request.bodySize = body.size();
    HttpResponse response;
    for (int attempt = 1; attempt <= kTries; ++attempt) {
        response = http_.Send(request);
        if (response.cancelled() || response.ok()) return response;
        std::chrono::milliseconds wait{0};
        if (response.status == 0 || response.status == 502 || response.status == 504) {
            wait = std::chrono::milliseconds(1500 * attempt * attempt);   // a dropped connection, a gateway hiccup
        } else if (response.status == 503) {
            wait = std::chrono::seconds(std::clamp(response.retryAfter, 5, 30));
        } else if (response.status == 429 && waitOutLimits) {
            const auto limit = std::chrono::seconds(std::max(response.retryAfter, 30));
            if (limit > kLongestWait) return response;
            const auto until = Clock::now() + limit;                // a visible countdown while SafeShare's limit runs out
            for (auto left = limit; left.count() > 0; left = std::chrono::duration_cast<std::chrono::seconds>(until - Clock::now())) {
                const int seconds = static_cast<int>(left.count());
                EditTransfer([seconds](ShareTransfer& t) { t.waiting = seconds; });
                if (!Pause(std::min<std::chrono::milliseconds>(1s, left))) return response;
            }
            EditTransfer([](ShareTransfer& t) { t.waiting = 0; });
            continue;
        } else {
            return response;                                       // the answer will not change by asking again
        }
        if (attempt < kTries && !Pause(wait)) return response;
    }
    return response;
}

void SafeShareService::DoSend(const ShareSendRequest& request) {
    std::wstring roomId;
    std::string token;
    std::vector<uint8_t> key;
    const auto failed = [&](const std::wstring& message) {
        // Nothing half-made is left behind. When Eflow is quitting, the next start closes it instead (it is saved unfinished).
        bool quitting = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quitting = stop_;
        }
        if (!roomId.empty() && !quitting) DoClose(roomId, true);
        EditTransfer([&](ShareTransfer& t) {
            t.phase = message.empty() ? SharePhase::Cancelled : SharePhase::Failed;
            t.error = message;
            t.waiting = 0;
        });
    };
    try {
        // 1. the files, as they are now
        std::vector<FileToSend> files;
        uint64_t total = 0;
        for (const std::wstring& path : request.paths) {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            const std::wstring name = fs::path(path).filename().wstring();
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                throw ShareError{L"Couldn't open " + name + L". Is it still there?"};
            const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            files.push_back({path, name, ContentType(path), size});
            total += size;
        }
        if (files.empty() || files.size() > kShareMaxFiles || total > kShareMaxBytes)
            throw ShareError{L"A SafeShare room holds 1 to 100 files, up to 4 GB in all."};
        if (cancel_) return failed({});
        EditTransfer([&](ShareTransfer& t) { t.total = total; });

        // 2. the room: password-protected, closes after 10 hours, opens on `maxDevices` devices at most
        key = sc::NewKey();
        if (key.empty()) throw ShareError{L"Couldn't make an encryption key on this PC."};
        std::wstring title = files.front().name;
        if (files.size() > 1) title += L" + " + std::to_wstring(files.size() - 1) + L" more";
        if (title.size() > 120) title = title.substr(0, 117) + L"...";
        const int64_t now = UnixNow(), expires = now + kShareRoomHours * 3600;
        Value room = Value::Object();
        room.Member("name") = Str(title);
        room.Member("passwordHash") = Str(sc::Sha256Hex(ToUtf8(request.password)));
        room.Member("authorName") = Str(request.sender.substr(0, kShareMaxName));
        room.Member("expiresAt") = Str(FormatIsoUtc(expires));
        room.Member("customExpiryHours") = Num(kShareRoomHours);
        room.Member("burnMode") = Bool(false);
        room.Member("maxUsers") = Num(std::clamp(request.maxDevices, 1, 999));
        const HttpResponse created = Api(L"POST", L"/api/rooms", json::Write(room), "", false);
        if (cancel_) return failed({});
        Value made;
        if (!created.ok() || !Parse(created.body, made) || Field(made, "id").empty() || Field(made, "author_token").empty())
            throw ShareError{created.ok() ? L"SafeShare answered in a way Eflow doesn't understand." : Explain(created)};
        roomId = FromUtf8(Field(made, "id"));
        token = Field(made, "author_token");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Room r;
            r.info.roomId = roomId;
            r.info.title = title;
            r.info.recipient = request.email;
            r.info.sentAt = now;
            r.info.expiresAt = expires;
            r.token = token;
            r.key = sc::Base64Url(key);
            rooms_.insert(rooms_.begin(), std::move(r));          // saved half-made: a crash now is cleaned up next start
            sender_ = request.sender;
            maxDevices_ = request.maxDevices;
            Save();
        }

        // 3. the files, encrypted here, one after another
        for (const FileToSend& f : files) {
            EditTransfer([&](ShareTransfer& t) {
                t.phase = SharePhase::Uploading;
                t.current = f.name;
            });
            Upload(f, roomId, token, key);
            if (cancel_) return failed({});
        }

        // 4. SafeShare's own email, with the link (the key is in the part after #)
        EditTransfer([](ShareTransfer& t) { t.phase = SharePhase::Inviting; });
        Value invite = Value::Object();
        invite.Member("email") = Str(request.email);
        invite.Member("roomId") = Str(roomId);
        invite.Member("shareLink") = Str(std::string(kSite) + "/room/" + ToUtf8(roomId) + "#key=" + sc::Base64Url(key));
        const HttpResponse invited = Api(L"POST", L"/api/invite", json::Write(invite), "", true);
        if (cancel_) return failed({});
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (Room& r : rooms_)
                if (r.info.roomId == roomId) r.complete = true, r.info.emailed = invited.ok();
            Save();
            nextWatch_ = Clock::now() + kWatchEvery;
            wake_.notify_all();
        }
        const std::wstring sentRoom = roomId;
        roomId.clear();                                            // from here on the room stays, whatever happens
        if (!invited.ok()) {
            Log("safeshare: the invite email failed (%d)", invited.status);
            throw ShareError{invited.status == 429
                                 ? L"The files are uploaded, but SafeShare sends only 3 emails every 10 minutes. Open Sent, copy the link and send it yourself."
                                 : L"The files are uploaded, but the email didn't go out. Open Sent, copy the link and send it yourself."};
        }
        Log("safeshare: sent %zu file(s), %llu bytes", files.size(), static_cast<unsigned long long>(total));
        EditTransfer([&](ShareTransfer& t) {
            t.phase = SharePhase::Sent;
            t.roomId = sentRoom;
            t.done = t.total;
            t.waiting = 0;
        });
    } catch (const ShareError& e) {
        if (cancel_) return failed({});
        Log("safeshare: send failed: %ls", e.message.c_str());
        failed(e.message);
    }
}

void SafeShareService::Upload(const FileToSend& file, const std::wstring& roomId, const std::string& token, const std::vector<uint8_t>& key) {
    const std::string room = ToUtf8(roomId);
    Value start = Value::Object();
    start.Member("roomId") = Str(room);
    start.Member("filename") = Str(file.name);
    start.Member("fileSize") = Num(static_cast<int64_t>(file.size));
    start.Member("contentType") = Str(file.contentType);
    start.Member("encryptionMode") = Str(sc::kMode);
    start.Member("encryptionChunkSize") = Num(static_cast<int64_t>(sc::kChunkBytes));
    start.Member("encryptionVersion") = Str(sc::kVersion);
    const HttpResponse started = Api(L"POST", L"/api/multipart-upload/initiate", json::Write(start), token, true);
    if (cancel_) return;
    Value began;
    if (!started.ok() || !Parse(started.body, began)) throw ShareError{Explain(started)};
    const std::string uploadId = Field(began, "uploadId"), fileKey = Field(began, "fileKey");
    if (uploadId.empty() || fileKey.empty()) throw ShareError{L"SafeShare answered in a way Eflow doesn't understand."};
    const auto abort = [&] {
        http_.Reset();
        Value stop = Value::Object();
        stop.Member("uploadId") = Str(uploadId);
        stop.Member("fileKey") = Str(fileKey);
        stop.Member("roomId") = Str(room);
        Api(L"POST", L"/api/multipart-upload/abort", json::Write(stop), token, false);
    };

    // Where each part goes: presigned upload URLs (valid an hour; an expired one is asked for again).
    const uint64_t parts = sc::ChunkCount(file.size);
    std::vector<std::wstring> urls(parts);
    std::mutex urlLock;
    const auto fetchUrls = [&](uint64_t first, uint64_t count) -> HttpResponse {
        Value ask = Value::Object();
        ask.Member("uploadId") = Str(uploadId);
        ask.Member("fileKey") = Str(fileKey);
        ask.Member("roomId") = Str(room);
        Value& numbers = ask.Member("partNumbers") = Value::Array();
        for (uint64_t i = first; i < first + count; ++i) numbers.items.push_back(Num(static_cast<int64_t>(i + 1)));
        HttpResponse r = Api(L"POST", L"/api/multipart-upload/get-part-urls", json::Write(ask), token, true);
        Value got;
        if (!r.ok() || !Parse(r.body, got)) return r;
        if (Value* list = got.Find("presignedUrls"); list && list->type == Value::Type::Array) {
            std::lock_guard<std::mutex> lock(urlLock);
            for (Value& u : list->items) {
                const int64_t n = IntField(u, "partNumber");
                if (n >= 1 && static_cast<uint64_t>(n) <= parts) urls[static_cast<size_t>(n - 1)] = FromUtf8(Field(u, "url"));
            }
        }
        return r;
    };
    if (const HttpResponse r = fetchUrls(0, parts); !r.ok() || std::any_of(urls.begin(), urls.end(), [](const std::wstring& u) { return u.empty(); })) {
        if (cancel_) return abort();
        abort();
        throw ShareError{r.ok() ? L"SafeShare didn't say where to upload." : Explain(r)};
    }

    // The parts, sealed and uploaded by a few threads at once; progress counts the file's own bytes.
    std::vector<std::wstring> etags(parts);
    std::atomic<uint64_t> next{0}, sentBytes{0};
    std::mutex errorLock;
    std::wstring error;
    const uint64_t base = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        return transfer_.done;
    }();
    auto speedFrom = Clock::now();
    uint64_t speedBytes = 0;
    std::mutex speedLock;
    const auto report = [&] {
        const uint64_t done = sentBytes.load();
        double rate = 0;
        {
            std::lock_guard<std::mutex> lock(speedLock);
            const double seconds = std::chrono::duration<double>(Clock::now() - speedFrom).count();
            if (seconds >= 1.0) {
                rate = (done - speedBytes) / seconds;
                speedFrom = Clock::now();
                speedBytes = done;
            }
        }
        EditTransfer([&](ShareTransfer& t) {
            t.done = base + done;
            if (rate > 0) t.bytesPerSecond = t.bytesPerSecond > 0 ? t.bytesPerSecond * 0.6 + rate * 0.4 : rate;
        });
    };
    const auto work = [&] {
        sc::ChunkCipher cipher(key);
        const HANDLE handle = OpenForReading(file.path);
        std::vector<uint8_t> plain, sealed;
        const auto give_up = [&](std::wstring why) {
            std::lock_guard<std::mutex> lock(errorLock);
            if (error.empty()) error = std::move(why);
        };
        if (!cipher.ok() || handle == INVALID_HANDLE_VALUE) {
            give_up(L"Couldn't open " + file.name + L". Is it still there?");
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            return;
        }
        for (uint64_t part; (part = next++) < parts;) {
            {
                std::lock_guard<std::mutex> lock(errorLock);
                if (!error.empty() || cancel_) break;
            }
            const uint64_t offset = part * sc::kChunkBytes;
            const size_t size = static_cast<size_t>(std::min<uint64_t>(sc::kChunkBytes, file.size - offset));
            plain.resize(size);
            if ((size && !ReadAt(handle, offset, plain.data(), size)) || !cipher.Seal(plain.data(), size, sealed)) {
                give_up(L"Couldn't read " + file.name + L". Was it changed or moved while sending?");
                break;
            }
            uint64_t counted = 0;                                  // this part's share of the progress so far
            HttpResponse put;
            for (int attempt = 1; attempt <= kTries; ++attempt) {
                HttpRequest r;
                r.method = L"PUT";
                {
                    std::lock_guard<std::mutex> lock(urlLock);
                    r.url = urls[part];
                }
                r.headers.emplace_back(L"Content-Type", L"application/octet-stream");
                r.body = sealed.data();
                r.bodySize = sealed.size();
                r.onSent = [&](size_t bytes) {
                    const uint64_t now = static_cast<uint64_t>(bytes) * size / std::max<size_t>(1, sealed.size());
                    if (now > counted) sentBytes += now - counted, counted = now;
                    report();
                };
                put = http_.Send(r);
                if (put.ok() || put.cancelled()) break;
                sentBytes -= counted;                              // this part starts over
                counted = 0;
                if (put.status == 403 && attempt < kTries) {       // the URL ran out (a slow upload): a fresh one
                    fetchUrls(part, 1);
                    continue;
                }
                if (put.status != 0 && put.status < 500) break;
                if (attempt < kTries && !Pause(std::chrono::milliseconds(1500 * attempt * attempt))) break;
            }
            if (!put.ok()) {
                if (!put.cancelled()) give_up(Explain(put));
                break;
            }
            if (counted < size) sentBytes += size - counted;
            etags[part] = put.etag;
            report();
        }
        CloseHandle(handle);
    };
    std::vector<std::thread> threads;
    for (uint64_t i = 0; i < std::min<uint64_t>(kUploadThreads, parts); ++i) threads.emplace_back(work);
    for (std::thread& t : threads) t.join();
    if (cancel_) return abort();
    if (!error.empty()) {
        abort();
        throw ShareError{error};
    }

    // Done: SafeShare puts the parts together. Parts are numbered from 1, ETags without quotes, as the website sends them.
    Value finish = Value::Object();
    finish.Member("uploadId") = Str(uploadId);
    finish.Member("fileKey") = Str(fileKey);
    Value& list = finish.Member("parts") = Value::Array();
    for (uint64_t i = 0; i < parts; ++i) {
        Value p = Value::Object();
        p.Member("PartNumber") = Num(static_cast<int64_t>(i + 1));
        p.Member("ETag") = Str(etags[i].empty() ? "part-" + std::to_string(i + 1) : ToUtf8(etags[i]));
        list.items.push_back(std::move(p));
    }
    finish.Member("roomId") = Str(room);
    finish.Member("filename") = Str(file.name);
    finish.Member("fileSize") = Num(static_cast<int64_t>(file.size));
    finish.Member("message") = Str(std::string());
    finish.Member("encryptionMode") = Str(sc::kMode);
    finish.Member("encryptionChunkSize") = Num(static_cast<int64_t>(sc::kChunkBytes));
    finish.Member("encryptionVersion") = Str(sc::kVersion);
    const HttpResponse done = Api(L"POST", L"/api/multipart-upload/complete", json::Write(finish), token, true);
    if (cancel_) return abort();
    if (!done.ok()) {
        abort();
        throw ShareError{Explain(done)};
    }
    EditTransfer([&](ShareTransfer& t) { t.done = base + file.size; });
}

void SafeShareService::DoClose(const std::wstring& roomId, bool quiet) {
    std::string token;
    std::wstring title, recipient;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Room& r : rooms_)
            if (r.info.roomId == roomId) token = r.token, title = r.info.title, recipient = r.info.recipient;
    }
    if (token.empty()) return;
    http_.Reset();
    const HttpResponse r = Api(L"DELETE", L"/api/delete-room/" + roomId, "", token, false);
    if (r.ok() || r.status == 404) {                               // closed (or it had closed by itself already)
        std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(rooms_, [&](const Room& room) { return room.info.roomId == roomId; });
        ++version_;
        Save();
        Changed();
        return;
    }
    Log("safeshare: closing a room failed (%d)", r.status);
    if (!quiet) Post({Event::Kind::CloseFailed, roomId, title, recipient, Explain(r)});
}

// Who opened or downloaded what was sent. One room per turn, every 30 seconds: with SafeShare's limit of 300 requests per
// 15 minutes per connection (shared with the website), this stays at about a tenth of it however many rooms there are.
void SafeShareService::Watch() {
    const int64_t now = UnixNow();
    std::wstring roomId;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t before = rooms_.size();
        std::erase_if(rooms_, [&](const Room& r) { return r.info.expiresAt <= now; });   // closed by SafeShare by now
        if (rooms_.size() != before) ++version_, Save(), Changed();
        std::vector<size_t> live;
        for (size_t i = 0; i < rooms_.size(); ++i)
            if (rooms_[i].complete) live.push_back(i);
        if (live.empty()) {
            nextWatch_ = Clock::time_point::max();
            return;
        }
        const Room& r = rooms_[live[watchTurn_++ % live.size()]];
        roomId = r.info.roomId;
        token = r.token;
        nextWatch_ = Clock::now() + kWatchEvery;
    }
    http_.Reset();
    HttpRequest request;
    request.url = std::wstring(kApi) + L"/api/access-logs/" + roomId;
    request.headers.emplace_back(L"x-author-token", FromUtf8(token));
    const HttpResponse r = http_.Send(request);
    if (r.status == 429) {                                         // the connection is over SafeShare's budget: back off
        std::lock_guard<std::mutex> lock(mutex_);
        nextWatch_ = Clock::now() + std::max<std::chrono::seconds>(std::chrono::seconds(r.retryAfter), 5min);
        return;
    }
    if (r.status == 403 || r.status == 404) {                      // closed on the website meanwhile
        std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(rooms_, [&](const Room& room) { return room.info.roomId == roomId; });
        ++version_, Save(), Changed();
        return;
    }
    if (!r.ok()) return;
    std::vector<Event> fresh;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto room = std::find_if(rooms_.begin(), rooms_.end(), [&](const Room& x) { return x.info.roomId == roomId; });
    if (room == rooms_.end()) return;
    const size_t seenBefore = room->seen.size();
    std::vector<ShareNews> news;
    if (!ApplyAccessLog(r.body, now, kFreshEvent, room->info, room->seen, news)) return;
    for (const ShareNews& n : news)
        fresh.push_back({n.opened ? Event::Kind::Opened : Event::Kind::Downloaded, room->info.roomId, room->info.title, room->info.recipient, n.where});
    if (room->seen.size() > kSeenKept) room->seen.erase(room->seen.begin(), room->seen.end() - kSeenKept);
    if (room->seen.size() != seenBefore) ++version_, Save(), Changed();
    for (Event& e : fresh) Post(std::move(e));
}

// ---- storage (%LOCALAPPDATA%\Eflow\safeshare.json; each room's token and key sealed with DPAPI) --------------------------

void SafeShareService::Load() {
    const fs::path path = StorePath();
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::stringstream text;
    text << in.rdbuf();
    Value v;
    if (!Parse(text.str(), v)) {
        Log("safeshare: %ls is unreadable; starting empty", path.c_str());
        return;
    }
    const int64_t now = UnixNow();
    sender_ = FromUtf8(Field(v, "sender"));
    maxDevices_ = static_cast<int>(std::clamp<int64_t>(IntField(v, "maxDevices"), 1, 999));
    if (Value* list = v.Find("rooms"); list && list->type == Value::Type::Array) {
        for (Value& item : list->items) {
            Room r;
            r.info.roomId = FromUtf8(Field(item, "id"));
            r.info.title = FromUtf8(Field(item, "title"));
            r.info.recipient = FromUtf8(Field(item, "recipient"));
            r.info.sentAt = IntField(item, "sentAt");
            r.info.expiresAt = IntField(item, "expiresAt");
            r.info.opens = static_cast<int>(IntField(item, "opens"));
            r.info.downloads = static_cast<int>(IntField(item, "downloads"));
            r.info.lastSeen = FromUtf8(Field(item, "lastSeen"));
            const Value* emailed = item.Find("emailed");
            r.info.emailed = !emailed || emailed->boolean;
            const Value* complete = item.Find("complete");
            r.complete = complete && complete->boolean;
            if (Value* seen = item.Find("seen"); seen && seen->type == Value::Type::Array)
                for (const Value& s : seen->items) r.seen.push_back(s.text);
            std::vector<uint8_t> blob;
            std::string secret;
            if (!sc::FromBase64Url(Field(item, "secret"), blob) ||
                !UnprotectForUser(std::string(blob.begin(), blob.end()), kSecretPurpose, secret))
                continue;                                          // (another user's, or damaged: it can't be used)
            const size_t split = secret.find('\n');
            if (split == std::string::npos || r.info.roomId.empty() || r.info.expiresAt <= now) continue;
            r.token = secret.substr(0, split);
            r.key = secret.substr(split + 1);
            SecureZeroMemory(secret.data(), secret.size());
            rooms_.push_back(std::move(r));
        }
    }
    for (const Room& r : rooms_) {
        if (r.complete) continue;                                  // a send Eflow didn't finish (it was closed or crashed)
        Job job;
        job.kind = Job::Kind::Close;
        job.roomId = r.info.roomId;
        job.quiet = true;
        jobs_.push_back(std::move(job));
    }
    if (std::any_of(rooms_.begin(), rooms_.end(), [](const Room& r) { return r.complete; })) nextWatch_ = Clock::now() + 5s;
}

void SafeShareService::Save() const {
    Value v = Value::Object();
    v.Member("sender") = Str(sender_);
    v.Member("maxDevices") = Num(maxDevices_);
    Value& list = v.Member("rooms") = Value::Array();
    for (const Room& r : rooms_) {
        std::string sealed;
        std::string secret = r.token + "\n" + r.key;
        const bool ok = ProtectForUser(secret, kSecretPurpose, sealed);
        SecureZeroMemory(secret.data(), secret.size());
        if (!ok) continue;
        Value item = Value::Object();
        item.Member("id") = Str(r.info.roomId);
        item.Member("title") = Str(r.info.title);
        item.Member("recipient") = Str(r.info.recipient);
        item.Member("sentAt") = Num(r.info.sentAt);
        item.Member("expiresAt") = Num(r.info.expiresAt);
        item.Member("opens") = Num(r.info.opens);
        item.Member("downloads") = Num(r.info.downloads);
        item.Member("lastSeen") = Str(r.info.lastSeen);
        item.Member("emailed") = Bool(r.info.emailed);
        item.Member("complete") = Bool(r.complete);
        Value& seen = item.Member("seen") = Value::Array();
        for (const std::string& s : r.seen) seen.items.push_back(Str(s));
        item.Member("secret") = Str(sc::Base64Url(std::vector<uint8_t>(sealed.begin(), sealed.end())));
        list.items.push_back(std::move(item));
    }
    const fs::path path = StorePath();
    if (path.empty()) return;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << json::Write(v);
        if (!out) return;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) Log("safeshare: could not save (%lu)", GetLastError());
}

} // namespace eflow
