#include "services/PhoneShare.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <filesystem>

#include "core/Json.h"
#include "core/Log.h"
#include "core/Utf8.h"
#include "qrcodegen.hpp"
#include "services/PhonePage.h"
#include "services/ShareCrypto.h"

namespace eflow {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

constexpr ULONGLONG kIdleMs = 10 * 60 * 1000;                     // no request for this long: the link ends by itself
constexpr DWORD kSocketTimeoutMs = 30000;
constexpr size_t kMaxHeaders = 16 * 1024;
constexpr size_t kMaxClients = 12;
constexpr uint64_t kMaxUpload = 4ull * 1024 * 1024 * 1024;
constexpr uint64_t kKeepFree = 512ull * 1024 * 1024;              // an upload never fills the disk to the brim
constexpr size_t kPiece = 1024 * 1024;

// ---- the local network --------------------------------------------------------------------------------------------------

bool IsPrivateIPv4(const in_addr& a) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&a);
    return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168);
}

// Windows' own judgement of the network the adapter is on: home/work (private, domain) or public.
bool IsPublicNetwork(const GUID& adapter) {
    ComPtr<INetworkListManager> nlm;
    ComPtr<IEnumNetworkConnections> connections;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm))) ||
        FAILED(nlm->GetNetworkConnections(&connections)))
        return false;                                             // (unknown: not treated as public)
    for (ComPtr<INetworkConnection> c; connections->Next(1, &c, nullptr) == S_OK; c.Reset()) {
        GUID id{};
        ComPtr<INetwork> network;
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PRIVATE;
        if (SUCCEEDED(c->GetAdapterId(&id)) && IsEqualGUID(id, adapter) && SUCCEEDED(c->GetNetwork(&network)) &&
            SUCCEEDED(network->GetCategory(&category)))
            return category == NLM_NETWORK_CATEGORY_PUBLIC;
    }
    return false;
}

// This PC's address on its Wi-Fi (or wired) network: an adapter that is up, has a gateway and a private IPv4 address.
// Virtual adapters (WSL, Hyper-V, VPN tunnels) have no gateway or are not Wi-Fi / Ethernet, so they are passed over.
// `isPublic`: Windows calls that network public (new Wi-Fi networks start out so, even at home).
bool FindLocalAddress(std::string& ip, std::wstring& reason, bool& isPublic) {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    }
    if (result != NO_ERROR) {
        reason = L"Windows didn't say which network this PC is on.";
        return false;
    }
    int best = 0;
    GUID bestAdapter{};
    for (const IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
        const int score = a->IfType == IF_TYPE_IEEE80211 ? 2 : a->IfType == IF_TYPE_ETHERNET_CSMACD ? 1 : 0;
        if (score <= best) continue;
        for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (v4->sin_family != AF_INET || !IsPrivateIPv4(v4->sin_addr)) continue;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text));
            ip = text;
            best = score;
            CLSIDFromString(FromUtf8(a->AdapterName).c_str(), &bestAdapter);
            break;
        }
    }
    if (!best) {
        reason = L"This PC isn't on a Wi-Fi or home network right now. Connect it to the same Wi-Fi as your phone.";
        return false;
    }
    isPublic = IsPublicNetwork(bestAdapter);
    return true;
}

// ---- HTTP -----------------------------------------------------------------------------------------------------------------

bool SendAll(SOCKET s, const char* data, size_t size) {
    while (size) {
        const int sent = send(s, data, static_cast<int>(std::min<size_t>(size, 64 * 1024)), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

void Respond(SOCKET s, int status, const char* contentType, const std::string& body, const std::string& extra = {}) {
    const char* reason = status == 200 ? "OK" : status == 301 ? "Moved Permanently" : status == 400 ? "Bad Request"
                       : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 411 ? "Length Required"
                       : status == 413 ? "Payload Too Large" : status == 507 ? "Insufficient Storage" : "Internal Server Error";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + contentType +
                       "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                       "Connection: close\r\n" + extra + "\r\n";
    if (SendAll(s, head.data(), head.size())) SendAll(s, body.data(), body.size());
}

void RespondError(SOCKET s, int status, const std::string& message) {
    json::Value v = json::Value::Object();
    v.Member("error") = json::Value::String(message);
    Respond(s, status, "application/json; charset=utf-8", json::Write(v, 0));
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string PercentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string PercentEncode(const std::string& s) {                  // for filename*= (RFC 5987)
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
        else out += '%', out += kHex[c >> 4], out += kHex[c & 15];
    }
    return out;
}

std::string QueryValue(const std::string& query, const char* key) {
    const std::string want = std::string(key) + "=";
    for (size_t at = 0; at <= query.size();) {
        const size_t end = std::min(query.find('&', at), query.size());
        if (query.compare(at, want.size(), want) == 0) return PercentDecode(query.substr(at + want.size(), end - at - want.size()));
        at = end + 1;
    }
    return {};
}

std::string CookieValue(const std::string& cookies, const char* key) {
    const std::string want = std::string(key) + "=";
    for (size_t at = 0; at < cookies.size();) {
        while (at < cookies.size() && (cookies[at] == ' ' || cookies[at] == ';')) ++at;
        const size_t end = std::min(cookies.find(';', at), cookies.size());
        if (cookies.compare(at, want.size(), want) == 0) return cookies.substr(at + want.size(), end - at - want.size());
        at = end;
    }
    return {};
}

std::wstring DeviceName(const std::string& userAgent, const std::string& ip) {
    std::wstring kind = L"A device";
    if (userAgent.find("iPhone") != std::string::npos) kind = L"An iPhone";
    else if (userAgent.find("iPad") != std::string::npos) kind = L"An iPad";
    else if (userAgent.find("Android") != std::string::npos) kind = L"An Android phone";
    return kind + L" at " + FromUtf8(ip);
}

// A name the phone gave, made safe for this PC: no folders, no characters Windows refuses, no reserved device names.
std::wstring SafeFileName(const std::string& utf8) {
    std::wstring name = FromUtf8(utf8);
    if (const size_t slash = name.find_last_of(L"\\/"); slash != std::wstring::npos) name = name.substr(slash + 1);
    for (wchar_t& c : name)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    if (name.size() > 180) {
        const std::wstring ext = fs::path(name).extension().wstring();
        name = name.substr(0, 180 - std::min<size_t>(ext.size(), 20)) + ext.substr(0, 20);
    }
    std::wstring stem = fs::path(name).stem().wstring();
    for (wchar_t& c : stem) c = static_cast<wchar_t>(towupper(c));
    static const wchar_t* const kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
                                               L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    if (std::any_of(std::begin(kReserved), std::end(kReserved), [&](const wchar_t* r) { return stem == r; })) name = L"_" + name;
    return name.empty() ? L"file" : name;
}

fs::path ReceiveFolder() {
    PWSTR base = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &base))) dir = fs::path(base) / L"From phone";
    CoTaskMemFree(base);
    return dir;
}

fs::path UniquePath(const fs::path& dir, const std::wstring& name) {
    const fs::path first = dir / name;
    std::error_code ec;
    if (!fs::exists(first, ec)) return first;
    const std::wstring stem = fs::path(name).stem().wstring(), ext = fs::path(name).extension().wstring();
    for (int n = 2; n < 10000; ++n) {
        const fs::path p = dir / (stem + L" (" + std::to_wstring(n) + L")" + ext);
        if (!fs::exists(p, ec)) return p;
    }
    return dir / (stem + L" (" + std::to_wstring(GetTickCount64()) + L")" + ext);
}

std::string RandomToken() {
    std::vector<uint8_t> bytes(16);
    return sharecrypto::RandomBytes(bytes.data(), bytes.size()) ? sharecrypto::Base64Url(bytes) : std::string();
}

} // namespace

// ---- lifetime ----------------------------------------------------------------------------------------------------------

PhoneShare::PhoneShare(HWND notify, UINT changed, UINT received)
    : notify_(notify), changedMessage_(changed), receivedMessage_(received), sessions_([] { return RandomToken(); }) {
    WSADATA data;
    wsa_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = ARRAYSIZE(name);
    if (GetComputerNameW(name, &size)) pcName_ = ToUtf8(name);
}

PhoneShare::~PhoneShare() {
    Stop();
    if (wsa_) WSACleanup();
}

void PhoneShare::Start(bool evenIfPublic) {
    std::lock_guard<std::mutex> control(controlLock_);
    Shutdown();                                                   // (a link that ended by itself: its threads are done)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        link_ = {};
        link_.state = PhoneState::Starting;
        sessions_.Reset();
    }
    Changed();
    stop_ = false;
    lastUse_ = GetTickCount64();
    server_ = std::thread([this, evenIfPublic] { Serve(evenIfPublic); });
}

void PhoneShare::Stop() {
    std::lock_guard<std::mutex> control(controlLock_);
    Shutdown();
    std::lock_guard<std::mutex> lock(mutex_);
    if (link_.state != PhoneState::Off) {
        link_ = {};
        Changed();
    }
}

void PhoneShare::Shutdown() {
    stop_ = true;
    if (server_.joinable()) server_.join();                       // it closes the listener and waits for its clients
}

void PhoneShare::Allow(bool allow) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (link_.state != PhoneState::Asking || !sessions_.Asking()) return;
    if (allow) {
        sessions_.Allow();
        link_.device = askingDevice_;
        link_.state = PhoneState::Connected;
        Log("phone: allowed %ls", askingDevice_.c_str());
    } else {
        sessions_.Deny();
        link_.device.clear();
        link_.state = PhoneState::Waiting;
    }
    Changed();
}

void PhoneShare::Offer(std::vector<std::wstring> paths) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paths == offered_) return;
    offered_ = std::move(paths);
    ++offerVersion_;                                              // the phone's old download links stop working
}

PhoneLink PhoneShare::Snapshot() const {
    posted_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    return link_;
}

void PhoneShare::Changed() {
    if (!posted_.exchange(true)) PostMessageW(notify_, changedMessage_, 0, 0);
}

void PhoneShare::SetState(PhoneState state, const std::wstring& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    link_.state = state;
    link_.reason = reason;
    if (state == PhoneState::Off || state == PhoneState::Unavailable) link_.qr.clear(), link_.qrSize = 0, link_.url.clear();
    link_.publicNetwork = false;
    Changed();
}

// ---- the server ----------------------------------------------------------------------------------------------------------

void PhoneShare::Serve(bool evenIfPublic) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // (the network list is a COM object)
    std::string ip;
    std::wstring reason;
    SOCKET listener = INVALID_SOCKET;
    const auto finish = [&] {
        if (listener != INVALID_SOCKET) closesocket(listener);
        {
            std::lock_guard<std::mutex> lock(clientsLock_);
            for (Client& c : clients_)
                if (c.socket != INVALID_SOCKET) shutdown(c.socket, SD_BOTH);   // a transfer in progress ends now
        }
        for (Client& c : clients_)
            if (c.thread.joinable()) c.thread.join();
        clients_.clear();
        if (SUCCEEDED(com)) CoUninitialize();
    };
    bool isPublic = false;
    if (!wsa_ || !FindLocalAddress(ip, reason, isPublic)) {
        SetState(PhoneState::Unavailable, reason.empty() ? L"Windows networking isn't available." : reason);
        return finish();
    }
    if (isPublic && !evenIfPublic) {                              // a café or hotel network: not without the user's word
        SetState(PhoneState::Unavailable, L"Windows calls this network public, like a café's. If it is your own Wi-Fi, choose "
                                         L"It's my Wi-Fi. (Or set it to Private in Settings, Network & internet.)");
        std::lock_guard<std::mutex> lock(mutex_);
        link_.publicNetwork = true;
        Changed();
        return finish();
    }
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_port = 0;                                              // a free port, chosen by Windows
    inet_pton(AF_INET, ip.c_str(), &at.sin_addr);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int length = sizeof(at);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0 || listen(listener, 8) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&at), &length) != 0) {
        Log("phone: could not listen on %s (%d)", ip.c_str(), WSAGetLastError());
        SetState(PhoneState::Unavailable, L"Eflow couldn't open a connection for the phone. Another program may be blocking it.");
        return finish();
    }
    const std::string token = RandomToken();
    const std::string url = "http://" + ip + ":" + std::to_string(ntohs(at.sin_port)) + "/s/" + token + "/";
    std::vector<uint8_t> modules;
    int side = 0;
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        side = qr.getSize();
        modules.resize(static_cast<size_t>(side) * static_cast<size_t>(side));
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x) modules[static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(x)] = qr.getModule(x, y);
    } catch (const std::exception&) {
        side = 0;
    }
    if (token.empty() || side == 0) {
        SetState(PhoneState::Unavailable, L"Eflow couldn't make the code for the phone.");
        return finish();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prefix_ = "/s/" + token;
        link_.url = FromUtf8(url);
        link_.qr = std::move(modules);
        link_.qrSize = side;
        link_.state = PhoneState::Waiting;
        link_.reason.clear();
        Changed();
    }
    Log("phone: waiting on %s:%u", ip.c_str(), ntohs(at.sin_port));

    while (!stop_) {
        if (GetTickCount64() - lastUse_.load() > kIdleMs) {         // left unused: it ends by itself
            Log("phone: idle, stopping");
            SetState(PhoneState::Off);
            break;
        }
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener, &ready);
        timeval wait{1, 0};
        if (select(0, &ready, nullptr, nullptr, &wait) <= 0) continue;
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const SOCKET s = accept(listener, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (s == INVALID_SOCKET) continue;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
        std::lock_guard<std::mutex> lock(clientsLock_);
        for (auto it = clients_.begin(); it != clients_.end();) {   // the finished ones
            if (it->done) {
                it->thread.join();
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        if (clients_.size() >= kMaxClients) {
            closesocket(s);
            continue;
        }
        Client& c = clients_.emplace_back();
        c.socket = s;
        const std::string peer = text;
        c.thread = std::thread([this, &c, s, peer] {
            Handle(s, peer);
            {
                std::lock_guard<std::mutex> closing(clientsLock_);   // (never shut down after it is closed)
                closesocket(s);
                c.socket = INVALID_SOCKET;
            }
            c.done = true;
        });
    }
    finish();
}

void PhoneShare::Handle(SOCKET s, const std::string& ip) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kSocketTimeoutMs), sizeof(kSocketTimeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&kSocketTimeoutMs), sizeof(kSocketTimeoutMs));
    std::string data;
    size_t end = std::string::npos;
    char buf[4096];
    while ((end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() > kMaxHeaders) return Respond(s, 400, "text/plain", "Too long");
        const int got = recv(s, buf, sizeof(buf), 0);
        if (got <= 0) return;
        data.append(buf, static_cast<size_t>(got));
    }
    Request r;
    const size_t lineEnd = data.find("\r\n");
    const std::string line = data.substr(0, lineEnd);
    const size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return Respond(s, 400, "text/plain", "Bad request");
    r.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (const size_t q = target.find('?'); q != std::string::npos) r.query = target.substr(q + 1), target.resize(q);
    r.path = target;
    for (size_t at = lineEnd + 2; at < end;) {
        const size_t next = data.find("\r\n", at);
        const std::string header = data.substr(at, next - at);
        at = next + 2;
        const size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = Lower(header.substr(0, colon));
        std::string value = header.substr(colon + 1);
        value.erase(0, value.find_first_not_of(' '));
        if (name == "content-length") r.contentLength = std::strtoull(value.c_str(), nullptr, 10), r.hasLength = true;
        else if (name == "cookie") r.cookie = value;
        else if (name == "user-agent") r.userAgent = value;
        else if (name == "transfer-encoding") return RespondError(s, 411, "Send the file with a known length.");
    }
    r.body = data.substr(end + 4);
    lastUse_ = GetTickCount64();
    Route(s, r, ip);
}

void PhoneShare::Route(SOCKET s, Request& r, const std::string& ip) {
    std::string prefix;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prefix = prefix_;
    }
    if (prefix.empty() || r.path.compare(0, prefix.size(), prefix) != 0) return Respond(s, 404, "text/plain", "Not found");
    const std::string rest = r.path.substr(prefix.size());
    const std::string session = CookieValue(r.cookie, "eflow");
    if (rest.empty()) return Respond(s, 301, "text/plain", "", "Location: " + prefix + "/\r\n");
    if (rest == "/" && r.method == "GET") {
        PhoneSessions::Visit visit;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            visit = sessions_.Arrive(session, ip, link_.state == PhoneState::Waiting);
            if (visit.knocked) {                                  // it asks to connect: the PC decides
                askingDevice_ = DeviceName(r.userAgent, ip);
                link_.device = askingDevice_;
                link_.state = PhoneState::Asking;
                Changed();
                Log("phone: %ls asks to connect", askingDevice_.c_str());
            }
        }
        return SendPage(s, visit.session, visit.newCookie);
    }
    if (rest == "/api/state" && r.method == "GET") return SendState(s, session, ip);
    if (rest.compare(0, 10, "/api/file/") == 0 && r.method == "GET") return SendFile(s, session, ip, PercentDecode(rest.substr(10)));
    if (rest == "/api/upload" && r.method == "POST") return ReceiveFile(s, r, session, ip);
    Respond(s, 404, "text/plain", "Not found");
}

void PhoneShare::SendPage(SOCKET s, const std::string& session, bool newSession) {
    std::string prefix;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prefix = prefix_;
    }
    std::string extra = "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                        "connect-src 'self'; img-src 'self' data:; form-action 'none'; frame-ancestors 'none'; base-uri 'none'\r\n"
                        "X-Frame-Options: DENY\r\n";
    if (newSession) extra += "Set-Cookie: eflow=" + session + "; Path=" + prefix + "/; HttpOnly; SameSite=Strict\r\n";
    Respond(s, 200, "text/html; charset=utf-8", kPhonePage, extra);
}

void PhoneShare::SendState(SOCKET s, const std::string& session, const std::string& ip) {
    json::Value v = json::Value::Object();
    std::lock_guard<std::mutex> lock(mutex_);
    using Status = PhoneSessions::Status;
    const Status status = sessions_.StatusOf(session, ip);
    v.Member("status") = json::Value::String(status == Status::Allowed ? "allowed" : status == Status::Waiting ? "waiting"
                                             : status == Status::Denied ? "denied" : "busy");
    v.Member("pc") = json::Value::String(pcName_);
    json::Value& files = v.Member("files") = json::Value::Array();
    if (status == Status::Allowed) {
        for (size_t i = 0; i < offered_.size(); ++i) {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (!GetFileAttributesExW(offered_[i].c_str(), GetFileExInfoStandard, &info)) continue;
            json::Value f = json::Value::Object();
            f.Member("id") = json::Value::String(std::to_string(offerVersion_) + "." + std::to_string(i));
            f.Member("name") = json::Value::String(ToUtf8(fs::path(offered_[i]).filename().wstring()));
            f.Member("size") = json::Value::Number(std::to_string((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow));
            files.items.push_back(std::move(f));
        }
    }
    Respond(s, 200, "application/json; charset=utf-8", json::Write(v, 0));
}

void PhoneShare::SendFile(SOCKET s, const std::string& session, const std::string& ip, const std::string& id) {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sessions_.IsAllowed(session, ip)) return RespondError(s, 403, "Not allowed");
        const size_t dot = id.find('.');
        if (dot != std::string::npos && id.substr(0, dot) == std::to_string(offerVersion_)) {
            const size_t index = std::strtoull(id.c_str() + dot + 1, nullptr, 10);
            if (index < offered_.size()) path = offered_[index];
        }
    }
    if (path.empty()) return RespondError(s, 404, "That file is no longer offered.");
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size{};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return RespondError(s, 404, "The file can't be read on the PC.");
    }
    const std::string name = ToUtf8(fs::path(path).filename().wstring());
    std::string ascii;
    for (char c : name) ascii += (static_cast<unsigned char>(c) < 0x80 && c != '"' && c != '\\' && c >= 0x20) ? c : '_';
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(size.QuadPart) +
                             "\r\nContent-Disposition: attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + PercentEncode(name) +
                             "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
    bool ok = SendAll(s, head.data(), head.size());
    std::vector<char> buffer(kPiece);
    for (int64_t left = size.QuadPart; ok && left > 0 && !stop_;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(std::min<int64_t>(left, kPiece)), &read, nullptr) || read == 0) break;
        ok = SendAll(s, buffer.data(), read);
        left -= read;
        lastUse_ = GetTickCount64();                              // (a long download is use)
    }
    CloseHandle(file);
}

void PhoneShare::ReceiveFile(SOCKET s, Request& r, const std::string& session, const std::string& ip) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sessions_.IsAllowed(session, ip)) return RespondError(s, 403, "Not allowed");
    }
    if (!r.hasLength) return RespondError(s, 411, "Send the file with a known length.");
    if (r.contentLength > kMaxUpload) return RespondError(s, 413, "Too large (4 GB at most)");
    const fs::path dir = ReceiveFolder();
    std::error_code ec;
    if (dir.empty() || (fs::create_directories(dir, ec), !fs::is_directory(dir, ec))) return RespondError(s, 500, "No folder to save in");
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(dir.c_str(), &free, nullptr, nullptr) && free.QuadPart < r.contentLength + kKeepFree)
        return RespondError(s, 507, "The PC's disk is too full");

    const std::wstring name = SafeFileName(QueryValue(r.query, "name"));
    const fs::path part = dir / (name + L"." + FromUtf8(RandomToken().substr(0, 8)) + L".part");
    const HANDLE file = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return RespondError(s, 500, "Couldn't save on the PC");
    uint64_t written = 0;
    bool ok = true;
    const auto write = [&](const char* data, size_t size) {
        DWORD done = 0;
        ok = ok && WriteFile(file, data, static_cast<DWORD>(size), &done, nullptr) && done == size;
        written += size;
    };
    if (!r.body.empty()) write(r.body.data(), static_cast<size_t>(std::min<uint64_t>(r.body.size(), r.contentLength)));
    std::vector<char> buffer(kPiece);
    while (ok && written < r.contentLength && !stop_) {
        const int got = recv(s, buffer.data(), static_cast<int>(std::min<uint64_t>(buffer.size(), r.contentLength - written)), 0);
        if (got <= 0) {
            ok = false;
            break;
        }
        write(buffer.data(), static_cast<size_t>(got));
        lastUse_ = GetTickCount64();
    }
    CloseHandle(file);
    if (!ok || written != r.contentLength) {
        DeleteFileW(part.c_str());
        return RespondError(s, 400, "The upload didn't finish");
    }
    fs::path final = UniquePath(dir, name);
    if (!MoveFileExW(part.c_str(), final.c_str(), 0)) {
        DeleteFileW(part.c_str());
        return RespondError(s, 500, "Couldn't save on the PC");
    }
    SetFileAttributesW(final.c_str(), FILE_ATTRIBUTE_NORMAL);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++link_.received;
        Changed();
    }
    auto* path = new std::wstring(final.wstring());
    if (!PostMessageW(notify_, receivedMessage_, 0, reinterpret_cast<LPARAM>(path))) delete path;
    Respond(s, 200, "application/json; charset=utf-8", "{\"ok\":true}");
}

} // namespace eflow
