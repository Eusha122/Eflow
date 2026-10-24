#include "services/SoluSetup.h"

#include <windows.h>
#include <dxgi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Download.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Messages.h"
#include "core/Utf8.h"

namespace eflow {
namespace fs = std::filesystem;
namespace {

constexpr wchar_t kPythonZipUrl[] = L"https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip";
constexpr wchar_t kGetPipUrl[] = L"https://bootstrap.pypa.io/get-pip.py";
constexpr wchar_t kOllamaInstallerUrl[] = L"https://ollama.com/download/OllamaSetup.exe";
constexpr char kOllamaHost[] = "127.0.0.1";
constexpr unsigned short kOllamaPort = 11434;

struct ModelFile {
    const wchar_t *name, *url;
    uint64_t approxBytes;
};
constexpr ModelFile kSpeechModels[] = {
    {L"kokoro-v1.0.onnx", L"https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/kokoro-v1.0.onnx", 330u * 1024 * 1024},
    {L"voices-v1.0.bin", L"https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin", 27u * 1024 * 1024},
    {L"silero_vad.onnx", L"https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx", 2u * 1024 * 1024},
};

// The ladder of brains Solu can run, smallest first, all from Ollama's own library. A PC that can only just manage
// the first one still gets a Solu that answers; a PC with a big card gets a much better one. `gb` is the download.
struct BrainChoice {
    const wchar_t *id, *label, *note;
    double gb;             // what Ollama downloads
    double overheadGb;     // context cache and runtime, on top of the weights, while it is answering
    bool cpuTolerable;     // honest about speed: bigger than this and a PC without a graphics card crawls
};
constexpr BrainChoice kBrains[] = {
    {L"qwen3:0.6b", L"Tiny · 0.6B", L"For older or low-memory PCs. Short, simple answers, but quick.", 0.52, 0.4, true},
    {L"qwen3:1.7b", L"Small · 1.7B", L"Runs on almost anything, including PCs without a graphics card.", 1.4, 0.5, true},
    {L"qwen3:4b-instruct", L"Light · 4B", L"The everyday choice: fast, and good enough for most questions.", 2.5, 0.7, true},
    {L"qwen3:8b", L"Balanced · 8B", L"Noticeably better answers. Wants a graphics card of its own.", 5.2, 1.0, false},
    {L"qwen3:14b", L"Capable · 14B", L"Thinks things through properly. For a roomy graphics card.", 9.3, 1.2, false},
    {L"qwen3:30b-a3b", L"Powerful · 30B", L"The best Solu can be, and still fast. For 24 GB cards.", 19.0, 1.6, false},
};
constexpr uint64_t kGb = 1024ull * 1024 * 1024;

std::wstring ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) { path.resize(n); return path; }
        path.resize(path.size() * 2);
    }
}

// Where solu\ (its Python source, already part of the app) lives: next to eflow.exe when installed, or the
// repository's own solu\ in a development build (two levels above build\debug).
fs::path SoluSourceDir() {
    const fs::path dir = fs::path(ExePath()).parent_path();
    for (const fs::path& candidate : {dir / L"solu", dir / L".." / L".." / L"solu"}) {
        std::error_code ec;
        if (fs::exists(candidate / L"solu_service.py", ec)) return fs::weakly_canonical(candidate, ec);
    }
    return {};
}

// Everything the setup downloads (the Python runtime and the speech models) goes here, not next to the app: an
// installed Eflow's own folder is read-only (it is under Program Files, or inside the Store package).
fs::path SoluDataDir() {
    PWSTR local = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = fs::path(local) / L"Eflow" / L"solu";
    CoTaskMemFree(local);
    return dir;
}

// A development checkout keeps its own venv and models inside solu\; honour those when they are there, so a machine
// set up with tools\setup_solu.ps1 keeps working untouched.
fs::path SoluRuntimeDir(const fs::path& source) {
    std::error_code ec;
    if (!source.empty() && fs::exists(source / L"venv" / L"Scripts" / L"python.exe", ec)) return source;
    return SoluDataDir();
}

fs::path SoluModelsDir(const fs::path& source) {
    std::error_code ec;
    if (!source.empty() && fs::exists(source / L"models" / L"kokoro-v1.0.onnx", ec)) return source / L"models";
    return SoluDataDir() / L"models";
}

bool ReadFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void WriteFileText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// The requirements Solu actually needs on this PC: the two CUDA packages only make sense (and are each hundreds of MB)
// with an NVIDIA GPU, so a PC without one skips them entirely.
std::vector<std::string> FilteredRequirements(const fs::path& source, bool hasDedicatedGpu) {
    std::string text;
    if (!ReadFile(source / L"requirements.txt", text)) return {};
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;
        if (!hasDedicatedGpu && line.rfind("nvidia-", 0) == 0) continue;
        out.push_back(line);
    }
    return out;
}

// A minimal NVML probe, independent of SystemMonitor (which only samples while the System tab is visible): is there
// an NVIDIA GPU, and how much memory does it have. NVML ships with the NVIDIA display driver.
bool NvidiaVram(uint64_t& bytes) {
    HMODULE lib = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return false;
    struct Guard {
        HMODULE h;
        ~Guard() { FreeLibrary(h); }
    } guard{lib};
    auto init = reinterpret_cast<int (*)()>(GetProcAddress(lib, "nvmlInit_v2"));
    auto shutdown = reinterpret_cast<int (*)()>(GetProcAddress(lib, "nvmlShutdown"));
    auto getHandle = reinterpret_cast<int (*)(unsigned, void**)>(GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2"));
    struct Memory { unsigned long long total, free, used; };
    auto getMem = reinterpret_cast<int (*)(void*, Memory*)>(GetProcAddress(lib, "nvmlDeviceGetMemoryInfo"));
    if (!init || !shutdown || !getHandle || !getMem) return false;
    if (init() != 0) return false;
    void* device = nullptr;
    bool ok = false;
    if (getHandle(0, &device) == 0) {
        Memory mem{};
        if (getMem(device, &mem) == 0) {
            bytes = mem.total;
            ok = true;
        }
    }
    shutdown();
    return ok;
}

// The same question for any card, not only NVIDIA's: the biggest dedicated video memory DXGI reports, and the name
// of that adapter. Integrated graphics report little or no dedicated memory, which is the honest answer here - they
// share system memory, so a model on them is a model on the processor.
bool DedicatedVram(uint64_t& bytes, std::wstring& name) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    bool found = false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;            // the WARP renderer is not a graphics card
        if (desc.DedicatedVideoMemory <= bytes) continue;
        bytes = desc.DedicatedVideoMemory;
        name = desc.Description;
        found = true;
    }
    return found;
}

uint64_t SystemRam() {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    return GlobalMemoryStatusEx(&mem) ? mem.ullTotalPhys : 0;
}

// Where the setup remembers which brain it pulled, so Solu asks for that one and not a bigger one it never downloaded.
fs::path ChosenModelFile() {
    const fs::path dir = SoluDataDir();
    return dir.empty() ? fs::path{} : dir / L"model.txt";
}

// ---- a small client for Ollama's local API (loopback only, so plain HTTP; handles the chunked responses its
// streaming endpoints use) --------------------------------------------------------------------------------------

struct WinsockGuard {
    bool ok;
    WinsockGuard() {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WinsockGuard() {
        if (ok) WSACleanup();
    }
};

class Socket {
public:
    SOCKET s = INVALID_SOCKET;
    ~Socket() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
    bool Connect(int timeoutMs) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kOllamaPort);
        inet_pton(AF_INET, kOllamaHost, &addr.sin_addr);
        connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(s, &writeSet);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &writeSet, nullptr, &tv) <= 0) return false;
        int err = 0;
        int len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) return false;
        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        return true;
    }
    bool SendAll(const std::string& data) {
        size_t at = 0;
        while (at < data.size()) {
            const int n = send(s, data.data() + at, static_cast<int>(data.size() - at), 0);
            if (n <= 0) return false;
            at += static_cast<size_t>(n);
        }
        return true;
    }
    // -1: error/closed, 0: timed out (try again), >0: bytes read.
    int RecvSome(char* buf, int cap, int timeoutMs) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s, &readSet);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        const int r = select(0, &readSet, nullptr, nullptr, &tv);
        if (r == 0) return 0;
        if (r < 0) return -1;
        const int n = recv(s, buf, cap, 0);
        return n <= 0 ? -1 : n;
    }
};

// true: the request reached Ollama and ran to completion (whatever it said; onLine saw every NDJSON line of the
// body). false: could not even connect, or was cancelled, or the connection dropped mid-response.
bool OllamaRequest(const std::string& method, const std::string& path, const std::string& body, const std::atomic<bool>* cancel,
                   int connectTimeoutMs, const std::function<void(const std::string&)>& onLine) {
    WinsockGuard winsock;
    if (!winsock.ok) return false;
    Socket sock;
    if (!sock.Connect(connectTimeoutMs)) return false;

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!body.empty()) request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
    if (!sock.SendAll(request)) return false;

    std::string buf;
    char chunk[16384];
    auto fill = [&]() -> bool {                   // true: got more bytes; false: closed, errored, or cancelled
        for (;;) {
            if (cancel && cancel->load()) return false;
            const int n = sock.RecvSome(chunk, sizeof(chunk), 300);
            if (n > 0) {
                buf.append(chunk, static_cast<size_t>(n));
                return true;
            }
            if (n < 0) return false;              // 0: timed out this round; loop and check cancel again
        }
    };
    auto readLine = [&]() -> std::string {         // blocks until \n or the connection ends; "" + buf empty: ended
        for (;;) {
            const size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return line;
            }
            if (!fill()) {
                std::string rest = buf;
                buf.clear();
                return rest;
            }
        }
    };

    std::string status = readLine();
    if (status.rfind("HTTP/1.1 200", 0) != 0 && status.rfind("HTTP/1.0 200", 0) != 0) return false;
    bool chunked = false;
    int64_t contentLength = -1;
    for (std::string header = readLine(); !header.empty(); header = readLine()) {
        std::string lower = header;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); });
        if (lower.rfind("transfer-encoding:", 0) == 0 && lower.find("chunked") != std::string::npos) chunked = true;
        if (lower.rfind("content-length:", 0) == 0) contentLength = std::stoll(lower.substr(lower.find(':') + 1));
    }

    std::string pending;                           // a body line, possibly split across chunks
    auto feedBody = [&](const char* data, size_t n) {
        pending.append(data, n);
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && onLine) onLine(line);
        }
    };

    if (chunked) {
        for (;;) {
            std::string sizeLine = readLine();
            if (sizeLine.empty() && buf.empty()) break;            // connection ended without a terminating 0 chunk
            const size_t semi = sizeLine.find(';');
            const long size = strtol((semi == std::string::npos ? sizeLine : sizeLine.substr(0, semi)).c_str(), nullptr, 16);
            if (size <= 0) break;
            std::string piece;
            while (piece.size() < static_cast<size_t>(size)) {
                const size_t need = static_cast<size_t>(size) - piece.size();
                if (buf.empty() && !fill()) return false;
                const size_t take = std::min(need, buf.size());
                piece.append(buf, 0, take);
                buf.erase(0, take);
            }
            feedBody(piece.data(), piece.size());
            readLine();                                             // the chunk's trailing \r\n
        }
    } else if (contentLength >= 0) {
        int64_t remaining = contentLength - static_cast<int64_t>(buf.size());
        feedBody(buf.data(), buf.size());
        buf.clear();
        while (remaining > 0) {
            if (!fill()) return false;
            const size_t take = std::min<size_t>(buf.size(), static_cast<size_t>(remaining));
            feedBody(buf.data(), take);
            buf.erase(0, take);
            remaining -= static_cast<int64_t>(take);
        }
    } else {
        feedBody(buf.data(), buf.size());
        buf.clear();
        while (fill()) {
            feedBody(buf.data(), buf.size());
            buf.clear();
        }
    }
    // A response with no trailing newline (a single JSON object, not newline-delimited) leaves its last line sitting
    // unflushed in `pending`; the chunked/streaming case (Ollama's pulls) always ends its lines with '\n' already.
    if (!pending.empty() && onLine) onLine(pending);
    return true;
}

bool OllamaReachable(int timeoutMs) {
    bool got = false;
    OllamaRequest("GET", "/api/version", "", nullptr, timeoutMs, [&](const std::string&) { got = true; });
    return got;
}

// Every model Ollama already has, in one request. Asking per model meant six round trips - and six 1.2 s timeouts
// when Ollama was slow to answer - which is what made the Solu step sit on "Checking this PC".
std::vector<std::string> OllamaInstalledModels() {
    std::vector<std::string> names;
    OllamaRequest("GET", "/api/tags", "", nullptr, 1200, [&](const std::string& line) {
        json::Value v;
        std::string error;
        if (!json::Parse(line, v, error) || v.type != json::Value::Type::Object) return;
        json::Value* models = v.Find("models");
        if (!models || models->type != json::Value::Type::Array) return;
        for (json::Value& m : models->items) {
            json::Value* name = m.type == json::Value::Type::Object ? m.Find("name") : nullptr;
            if (name && name->type == json::Value::Type::String) names.push_back(name->text);
        }
    });
    return names;
}

bool OllamaHasModel(const std::wstring& modelId) {
    const std::string want = ToUtf8(modelId);
    bool found = false;
    OllamaRequest("GET", "/api/tags", "", nullptr, 1200, [&](const std::string& line) {
        json::Value v;
        std::string error;
        if (!json::Parse(line, v, error) || v.type != json::Value::Type::Object) return;
        json::Value* models = v.Find("models");
        if (!models || models->type != json::Value::Type::Array) return;
        for (json::Value& m : models->items) {
            json::Value* name = m.type == json::Value::Type::Object ? m.Find("name") : nullptr;
            if (name && name->type == json::Value::Type::String && (name->text == want || name->text == want + ":latest")) found = true;
        }
    });
    return found;
}

// `ollama.exe`, wherever OllamaInstalled() found it (PATH, or its default install folder).
std::wstring FindOllamaExe() {
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"ollama.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    PWSTR local = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        const fs::path candidate = fs::path(local) / L"Programs" / L"Ollama" / L"ollama.exe";
        std::error_code ec;
        if (fs::exists(candidate, ec)) path = candidate.wstring();
    }
    CoTaskMemFree(local);
    return path;
}

// A copy of this process's environment with PYTHONPATH/PYTHONHOME removed and PYTHONNOUSERSITE set, so a python.exe
// started with it never sees this PC's other Python installs: not their packages (a user-wide site-packages, found
// through "import site" the way pip itself needs it), not a stray PYTHONPATH pointing at some unrelated project.
// Without this, solu\pyruntime is not actually isolated, only appears to be.
std::vector<wchar_t> IsolatedPythonEnv() {
    std::vector<wchar_t> out;
    if (LPWCH block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p;) {
            const size_t len = wcslen(p);
            const std::wstring entry(p, len);
            const size_t eq = entry.find(L'=');
            const std::wstring name = eq == std::wstring::npos ? entry : entry.substr(0, eq);
            if (_wcsicmp(name.c_str(), L"PYTHONPATH") != 0 && _wcsicmp(name.c_str(), L"PYTHONHOME") != 0) {
                out.insert(out.end(), p, p + len + 1);
            }
            p += len + 1;
        }
        FreeEnvironmentStringsW(block);
    }
    const std::wstring extra = L"PYTHONNOUSERSITE=1";
    out.insert(out.end(), extra.begin(), extra.end());
    out.push_back(L'\0');
    out.push_back(L'\0');                 // the block's own terminator
    return out;
}

// Runs `exe args...` to completion, in a job object so Cancel (via *cancel) can kill it and anything it started.
// Output goes to `logPath` for diagnosis; returns the exit code, or -1 if it could not be started / was cancelled.
// `isolatedPython`: true for anything that runs solu\pyruntime's python.exe (see IsolatedPythonEnv).
// What pip is busy with, read from the log it is writing: "Collecting faster-whisper" / "Downloading numpy-2.4.6..."
// / "Installing collected packages: ...". Returns "" when there is nothing worth showing yet. Without this the step
// is a sweeping bar with no sign of life, and a few minutes of that looks exactly like a freeze.
std::wstring PipStep(const fs::path& logPath) {
    std::string text;
    {
        HANDLE f = CreateFileW(logPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        constexpr LONG tail = 8192;                       // only the end of it matters
        if (size.QuadPart > tail) SetFilePointer(f, -tail, nullptr, FILE_END);
        char buf[tail];
        DWORD got = 0;
        if (::ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) text.assign(buf, got);   // (::, not our own helper)
        CloseHandle(f);
    }
    // pip redraws its progress bars with \r, so split on both.
    std::string best;
    for (size_t at = 0, start = 0; at <= text.size(); ++at) {
        if (at != text.size() && text[at] != '\n' && text[at] != '\r') continue;
        const std::string line = text.substr(start, at - start);
        start = at + 1;
        for (const char* prefix : {"Collecting ", "Downloading ", "Installing collected packages", "Building wheel"})
            if (line.rfind(prefix, 0) == 0) best = line;
    }
    if (best.empty()) return {};
    if (best.rfind("Installing collected packages", 0) == 0) return L"Installing Python packages, almost there...";

    std::string name;
    if (best.rfind("Building wheel", 0) == 0) {                 // "Building wheel for ctranslate2 (setup.py)"
        const size_t at = best.find("for ");
        if (at == std::string::npos) return {};
        name = best.substr(at + 4);
        name.resize(std::min(name.size(), name.find(' ')));
    } else if (best.rfind("Collecting ", 0) == 0) {             // "Collecting faster-whisper==1.2.1" (a hyphen is part of it)
        name = best.substr(11);
        name.resize(std::min(name.size(), name.find_first_of("=<>[; ")));
    } else {                                                     // "Downloading faster_whisper-1.2.1-py3-none-any.whl (16 kB)"
        name = best.substr(best.find(' ') + 1);
        for (size_t at = 0; at + 1 < name.size(); ++at)
            if (name[at] == '-' && isdigit(static_cast<unsigned char>(name[at + 1]))) { name.resize(at); break; }
        name.resize(std::min(name.size(), name.find(' ')));
    }
    if (name.empty()) return {};
    return L"Installing Python packages: " + FromUtf8(name);
}

// `onTick` (optional) is called a few times a second while the tool runs, so a long step can say what it is doing.
int RunTool(const std::wstring& exe, const std::wstring& args, const fs::path& workDir, const fs::path& logPath, const std::atomic<bool>* cancel,
           bool isolatedPython = false, const std::function<void()>& onTick = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = si.hStdError = log;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> env = isolatedPython ? IsolatedPythonEnv() : std::vector<wchar_t>();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (isolatedPython ? CREATE_UNICODE_ENVIRONMENT : 0),
                                   isolatedPython ? env.data() : nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    CloseHandle(log);
    CloseHandle(in);
    if (!ok) return -1;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, pi.hProcess);
    }
    CloseHandle(pi.hThread);
    for (;;) {
        if (cancel && cancel->load()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        if (onTick) onTick();
    }
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return static_cast<int>(code);
}

} // namespace

SoluSetup::~SoluSetup() {
    Cancel();
    if (thread_.joinable()) {
        if (running_) thread_.detach();         // a network call may be mid-timeout: do not hold up shutdown for it
        else thread_.join();
    }
}

uint64_t SoluSetupPlan::EstimateBytes(const std::wstring& modelId) const {
    uint64_t total = 0;
    if (!pythonReady) total += 60u * 1024 * 1024;   // the runtime + packages (CPU-only; more with a CUDA download)
    if (!speechModelsReady)
        for (const ModelFile& m : kSpeechModels) total += m.approxBytes;
    if (!ollamaInstalled) total += 700u * 1024 * 1024;
    for (const SoluModelChoice& m : models)
        if (m.id == modelId && !m.ready) total += m.approxBytes;
    return total;
}

void SoluSetup::RateModels(SoluSetupPlan& plan) const {
    plan.models.clear();
    // Never recommend a model that would take the whole card. Solu shares it with everything else the user has open,
    // and a card with nothing left crashes THEM (while testing, a driver reset took VS Code down with it). The
    // reserve is the one solu\gpu.py already runs with - 1.5 GB, or more on a big card - so a recommendation here
    // means the brain really does fit whole, with the desktop, a browser and an editor still drawing.
    const double cardBytes = static_cast<double>(plan.vramBytes);
    // Two limits, whichever is stricter: a fixed reserve (small cards), and never more than about three quarters of
    // the card (big ones, where a tenth is not enough to game or edit video beside it). The 8B brain measured 6.2 GB
    // loaded whole (solu\gpu.py), and that is what an 8 GB card has been running all along - so it must still qualify.
    const double vramRoom = plan.hasDedicatedGpu ? (std::min)(cardBytes - (std::max)(1.5 * kGb, cardBytes * 0.10), cardBytes * 0.82) : 0.0;
    // With no card the processor answers, out of system memory - and it has to leave plenty for Windows and the apps.
    const double ramRoom = static_cast<double>(plan.ramBytes) * 0.35;
    for (const BrainChoice& b : kBrains) {
        SoluModelChoice m;
        m.id = b.id;
        m.label = b.label;
        m.note = b.note;
        m.approxBytes = static_cast<uint64_t>(b.gb * kGb);
        m.needBytes = static_cast<uint64_t>((b.gb + b.overheadGb) * kGb);
        const double need = static_cast<double>(m.needBytes);
        if (plan.hasDedicatedGpu) {
            if (need <= vramRoom) {
                m.fit = SoluModelFit::Comfortable;
                m.fitNote = L"Fits on your graphics card, with room left over";
            } else if (need <= cardBytes) {
                m.fit = SoluModelFit::Tight;
                m.fitNote = L"Would fill your card, leaving little for other apps";
            } else if (need <= ramRoom + vramRoom) {
                m.fit = SoluModelFit::Tight;
                m.fitNote = L"Part of it would run on the processor, slowly";
            } else {
                m.fit = SoluModelFit::TooBig;
                m.fitNote = L"Needs more memory than this PC has";
            }
        } else if (b.cpuTolerable && need <= ramRoom) {
            m.fit = SoluModelFit::Comfortable;
            m.fitNote = L"Runs on the processor, with memory to spare";
        } else if (need <= ramRoom) {
            m.fit = SoluModelFit::Tight;
            m.fitNote = L"Runs, but answers slowly without a graphics card";
        } else if (need <= static_cast<double>(plan.ramBytes)) {
            m.fit = SoluModelFit::Tight;
            m.fitNote = L"Would use most of this PC's memory";
        } else {
            m.fit = SoluModelFit::TooBig;
            m.fitNote = L"Needs more memory than this PC has";
        }
        plan.models.push_back(m);
    }
    plan.recommended = 0;                                   // the largest comfortable one; the smallest if none is
    for (size_t i = 0; i < plan.models.size(); ++i)
        if (plan.models[i].fit == SoluModelFit::Comfortable) plan.recommended = static_cast<int>(i);
}

SoluSetupPlan SoluSetup::Detect() const {
    SoluSetupPlan plan;
    const fs::path source = SoluSourceDir();
    std::error_code ec;
    if (!source.empty()) {
        const fs::path reqs = source / L"requirements.txt";
        std::string currentReqs;
        ReadFile(reqs, currentReqs);
        const fs::path runtime = SoluRuntimeDir(source);
        if (fs::exists(source / L"venv" / L"Scripts" / L"python.exe", ec)) {
            plan.pythonReady = true;                                  // a development venv: trust it as-is
        } else if (fs::exists(runtime / L"pyruntime" / L"python.exe", ec)) {
            std::string marker;
            plan.pythonReady = ReadFile(runtime / L"pyruntime" / L".packages-installed", marker) && marker == currentReqs && !currentReqs.empty();
        }
        const fs::path models = SoluModelsDir(source);
        plan.speechModelsReady = true;
        for (const ModelFile& m : kSpeechModels) plan.speechModelsReady &= fs::exists(models / m.name, ec);
    }
    plan.ollamaInstalled = !FindOllamaExe().empty();
    plan.ollamaRunning = plan.ollamaInstalled && OllamaReachable(400);
    plan.ramBytes = SystemRam();
    uint64_t vram = 0;
    std::wstring gpu;
    plan.hasDedicatedGpu = DedicatedVram(vram, gpu) && vram >= 2 * kGb;   // under 2 GB is integrated graphics in all but name
    if (!plan.hasDedicatedGpu) {                                          // no DXGI answer: NVML still knows its own cards
        uint64_t nv = 0;
        if (NvidiaVram(nv) && nv >= 2 * kGb) {
            plan.hasDedicatedGpu = true;
            vram = nv;
            if (gpu.empty()) gpu = L"your graphics card";
        }
    }
    plan.vramBytes = plan.hasDedicatedGpu ? vram : 0;
    plan.gpuName = plan.hasDedicatedGpu ? gpu : std::wstring();

    RateModels(plan);
    if (plan.ollamaRunning) {                                  // which of the ladder this PC already downloaded
        const std::vector<std::string> have = OllamaInstalledModels();
        for (SoluModelChoice& m : plan.models) {
            const std::string want = ToUtf8(m.id);
            m.ready = std::find(have.begin(), have.end(), want) != have.end() ||
                      std::find(have.begin(), have.end(), want + ":latest") != have.end();
        }
    }
    Log("solu setup: %ls, %.1f GB of memory -> recommending %ls", plan.hasDedicatedGpu ? plan.gpuName.c_str() : L"no dedicated graphics card",
        static_cast<double>(plan.ramBytes) / kGb, plan.models[static_cast<size_t>(plan.recommended)].id.c_str());
    return plan;
}

std::wstring SoluChosenModel() {
    const fs::path file = ChosenModelFile();
    std::string text;
    if (file.empty() || !ReadFile(file, text)) return {};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return FromUtf8(text);
}

void SoluSetup::Begin(HWND notify, const std::wstring& modelId) {
    if (running_) return;
    cancel_ = false;
    running_ = true;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, notify, modelId] { Run(notify, modelId); });
}

void SoluSetup::Cancel() { cancel_ = true; }

SoluSetupProgress SoluSetup::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void SoluSetup::Report(HWND notify, SoluSetupStage stage, const std::wstring& label, uint64_t done, uint64_t total) {
    SoluSetupProgress p;
    p.stage = stage;
    p.label = label;
    p.doneBytes = done;
    p.totalBytes = total;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = p;
    }
    PostMessageW(notify, WM_APP_SOLU_SETUP, 0, 0);
}

void SoluSetup::Finish(HWND notify, bool ok, const std::wstring& error) {
    SoluSetupProgress p;
    p.stage = ok ? SoluSetupStage::Done : SoluSetupStage::Failed;
    p.finished = true;
    p.ok = ok;
    p.error = error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = p;
    }
    Log(ok ? "solu setup: done" : "solu setup: failed (%ls)", error.c_str());
    running_ = false;
    PostMessageW(notify, WM_APP_SOLU_SETUP, 0, 0);
}

void SoluSetup::Run(HWND notify, std::wstring modelId) {
    const fs::path source = SoluSourceDir();
    if (source.empty()) return Finish(notify, false, L"Eflow's solu\\ folder is missing from this install.");
    std::error_code ec;
    uint64_t vram = 0;
    const bool hasDedicatedGpu = NvidiaVram(vram);

    // ---- the Python runtime (never the system Python: a private copy of our own) -------------------------------
    const fs::path py = SoluRuntimeDir(source) / L"pyruntime";
    const fs::path pyExe = py / L"python.exe";
    std::string currentReqs;
    ReadFile(source / L"requirements.txt", currentReqs);
    const bool devVenv = fs::exists(source / L"venv" / L"Scripts" / L"python.exe", ec);
    std::string marker;
    const bool packagesCurrent = ReadFile(py / L".packages-installed", marker) && marker == currentReqs && !currentReqs.empty();
    if (!devVenv && !(fs::exists(pyExe, ec) && packagesCurrent)) {
        if (cancel_) return Finish(notify, false, L"Cancelled");
        Report(notify, SoluSetupStage::PythonRuntime, L"Setting up Solu's Python runtime...");
        const fs::path zip = py / L"python.zip";
        fs::create_directories(py, ec);
        const DownloadResult dl = DownloadFile(kPythonZipUrl, zip.wstring(), &cancel_,
                                               [&](uint64_t done, uint64_t total) { Report(notify, SoluSetupStage::PythonRuntime, L"Setting up Solu's Python runtime...", done, total); });
        if (!dl.ok) return Finish(notify, false, dl.cancelled ? L"Cancelled" : L"Could not download the Python runtime.");

        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        const std::wstring tar = std::wstring(sysDir) + L"\\tar.exe";
        if (RunTool(tar, L"-xf \"" + zip.wstring() + L"\" -C \"" + py.wstring() + L"\"", {}, py / L"extract.log", &cancel_) != 0)
            return Finish(notify, false, L"Could not unpack the Python runtime.");
        fs::remove(zip, ec);

        // Enable site-packages: embeddable Python ships with it commented out in its ._pth file.
        for (const auto& entry : fs::directory_iterator(py, ec)) {
            if (entry.path().extension() == L"._pth") {
                std::string pth;
                ReadFile(entry.path(), pth);
                const std::string marker1 = "#import site";
                if (const size_t at = pth.find(marker1); at != std::string::npos) pth.replace(at, marker1.size(), "import site");
                if (pth.find("Lib\\site-packages") == std::string::npos) pth += "\nLib\\site-packages\n";
                WriteFileText(entry.path(), pth);
            }
        }
        if (cancel_) return Finish(notify, false, L"Cancelled");

        Report(notify, SoluSetupStage::PythonRuntime, L"Setting up pip...");
        const fs::path getPip = py / L"get-pip.py";
        if (!DownloadFile(kGetPipUrl, getPip.wstring(), &cancel_, nullptr).ok) return Finish(notify, false, L"Could not set up pip.");
        if (RunTool(pyExe.wstring(), L"get-pip.py --no-warn-script-location", py, py / L"get-pip.log", &cancel_, true) != 0)
            return Finish(notify, false, L"Could not set up pip.");
        fs::remove(getPip, ec);

        if (cancel_) return Finish(notify, false, L"Cancelled");
        Report(notify, SoluSetupStage::PythonPackages, L"Installing Python packages (a few minutes)...");
        const std::vector<std::string> packages = FilteredRequirements(source, hasDedicatedGpu);
        std::wstring args = L"-m pip install --no-warn-script-location";
        for (const std::string& pkg : packages) args += L" " + FromUtf8(pkg);
        const fs::path pipLog = py / L"pip-install.log";
        std::wstring lastStep;
        auto tick = [&] {                                     // name what pip is on, so the wait shows signs of life
            std::wstring step = PipStep(pipLog);
            if (step.empty() || step == lastStep) return;
            lastStep = step;
            Report(notify, SoluSetupStage::PythonPackages, step);
        };
        if (RunTool(pyExe.wstring(), args, py, pipLog, &cancel_, true, tick) != 0)
            return Finish(notify, false, L"Installing Solu's Python packages failed (see solu\\pyruntime\\pip-install.log).");
        WriteFileText(py / L".packages-installed", currentReqs);
    }
    if (cancel_) return Finish(notify, false, L"Cancelled");

    // ---- the speech models (Whisper downloads its own the first time Solu starts; these three do not) ----------
    const fs::path models = SoluModelsDir(source);
    fs::create_directories(models, ec);
    for (const ModelFile& m : kSpeechModels) {
        const fs::path dest = models / m.name;
        if (fs::exists(dest, ec)) continue;
        if (cancel_) return Finish(notify, false, L"Cancelled");
        const std::wstring label = std::wstring(L"Downloading ") + m.name + L"...";
        Report(notify, SoluSetupStage::SpeechModels, label);
        const DownloadResult dl = DownloadFile(m.url, dest.wstring(), &cancel_,
                                               [&](uint64_t done, uint64_t total) { Report(notify, SoluSetupStage::SpeechModels, label, done, total ? total : m.approxBytes); });
        if (!dl.ok) return Finish(notify, false, dl.cancelled ? L"Cancelled" : (std::wstring(L"Could not download ") + m.name));
    }

    // ---- Ollama: the local server that runs the language model ---------------------------------------------------
    std::wstring ollamaExe = FindOllamaExe();
    if (ollamaExe.empty()) {
        if (cancel_) return Finish(notify, false, L"Cancelled");
        Report(notify, SoluSetupStage::Ollama, L"Downloading Ollama...");
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        const fs::path installer = fs::path(tempDir) / L"EflowOllamaSetup.exe";
        const DownloadResult dl = DownloadFile(kOllamaInstallerUrl, installer.wstring(), &cancel_,
                                               [&](uint64_t done, uint64_t total) { Report(notify, SoluSetupStage::Ollama, L"Downloading Ollama...", done, total); });
        if (!dl.ok) return Finish(notify, false, dl.cancelled ? L"Cancelled" : L"Could not download Ollama.");
        if (cancel_) return Finish(notify, false, L"Cancelled");
        Report(notify, SoluSetupStage::Ollama, L"Installing Ollama...");
        const int code = RunTool(installer.wstring(), L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART", {}, fs::path(tempDir) / L"ollama-install.log", &cancel_);
        fs::remove(installer, ec);
        if (code != 0) return Finish(notify, false, cancel_ ? L"Cancelled" : L"Ollama's installer reported a problem.");
        ollamaExe = FindOllamaExe();
        if (ollamaExe.empty()) return Finish(notify, false, L"Ollama was installed but could not be found afterwards.");
    }
    if (!OllamaReachable(400)) {
        Report(notify, SoluSetupStage::OllamaStarting, L"Starting Ollama...");
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        // "serve" is what starts the local server. Plain `ollama.exe` is the CLI: with no arguments it shows its own
        // menu and waits, so it never listens - which looked exactly like Ollama failing to start.
        std::wstring cmd = L"\"" + ollamaExe + L"\" serve";
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        for (int i = 0; i < 120 && !cancel_; ++i) {      // a first start reads its models off a cold disk: give it a minute
            if (OllamaReachable(400)) break;
            Sleep(500);
        }
        if (cancel_) return Finish(notify, false, L"Cancelled");
        if (!OllamaReachable(400)) return Finish(notify, false, L"Ollama installed, but its local service did not start.");
    }

    // ---- the language model itself, pulled through Ollama's own API (the same thing `ollama pull` does) ----------
    if (!OllamaHasModel(modelId)) {
        if (cancel_) return Finish(notify, false, L"Cancelled");
        const std::wstring label = L"Downloading the language model (" + modelId + L")...";
        Report(notify, SoluSetupStage::LanguageModel, label);
        json::Value req = json::Value::Object();
        req.Member("name") = json::Value::String(ToUtf8(modelId));
        req.Member("stream").type = json::Value::Type::Bool;
        req.Member("stream").boolean = true;
        bool success = false, failed = false;
        std::wstring pullError;
        const bool reached = OllamaRequest("POST", "/api/pull", json::Write(req, 0), &cancel_, 2000, [&](const std::string& line) {
            json::Value v;
            std::string error;
            if (!json::Parse(line, v, error) || v.type != json::Value::Type::Object) return;
            if (json::Value* e = v.Find("error")) {
                failed = true;
                pullError = FromUtf8(e->type == json::Value::Type::String ? e->text : "Ollama reported an error");
                return;
            }
            json::Value* st = v.Find("status");
            if (st && st->type == json::Value::Type::String && st->text == "success") success = true;
            json::Value* completed = v.Find("completed");
            json::Value* total = v.Find("total");
            if (completed && total && completed->type == json::Value::Type::Number && total->type == json::Value::Type::Number)
                Report(notify, SoluSetupStage::LanguageModel, label, static_cast<uint64_t>(std::stod(completed->text)),
                      static_cast<uint64_t>(std::stod(total->text)));
        });
        if (!reached) return Finish(notify, false, cancel_ ? L"Cancelled" : L"Lost the connection to Ollama while downloading the model.");
        if (failed) return Finish(notify, false, pullError);
        if (!success) return Finish(notify, false, L"Ollama ended the download without confirming it finished.");
    }

    // Remember which brain this PC actually has, so Solu asks Ollama for that one and never for a bigger one.
    if (const fs::path chosen = ChosenModelFile(); !chosen.empty() && !modelId.empty()) {
        std::error_code dirEc;
        fs::create_directories(chosen.parent_path(), dirEc);
        std::ofstream(chosen, std::ios::binary) << ToUtf8(modelId);
    }
    Finish(notify, true, L"");
}

} // namespace eflow
