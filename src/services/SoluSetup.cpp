#include "services/SoluSetup.h"

#include <windows.h>
#include <shlobj.h>
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
std::vector<std::string> FilteredRequirements(const fs::path& source, bool hasNvidiaGpu) {
    std::string text;
    if (!ReadFile(source / L"requirements.txt", text)) return {};
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;
        if (!hasNvidiaGpu && line.rfind("nvidia-", 0) == 0) continue;
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
int RunTool(const std::wstring& exe, const std::wstring& args, const fs::path& workDir, const fs::path& logPath, const std::atomic<bool>* cancel,
           bool isolatedPython = false) {
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
    plan.hasNvidiaGpu = NvidiaVram(plan.vramBytes);

    SoluModelChoice capable{L"qwen3:8b", L"Capable (8B)", L"The fuller model: better answers, needs a decent graphics card.", 5200ull * 1024 * 1024};
    SoluModelChoice lighter{L"qwen3:4b-instruct", L"Lighter (4B)", L"Smaller and faster; works on most PCs, including without a dedicated graphics card.",
                            2600ull * 1024 * 1024};
    if (plan.ollamaRunning) {
        capable.ready = OllamaHasModel(capable.id);
        lighter.ready = OllamaHasModel(lighter.id);
    }
    plan.models = {capable, lighter};
    // 12 GB+ is comfortable for an 8B model alongside everything else on the GPU; below that, the lighter model stays responsive.
    plan.recommended = plan.hasNvidiaGpu && plan.vramBytes >= 12ull * 1024 * 1024 * 1024 ? 0 : 1;
    return plan;
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
    const bool hasNvidiaGpu = NvidiaVram(vram);

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
        const std::vector<std::string> packages = FilteredRequirements(source, hasNvidiaGpu);
        std::wstring args = L"-m pip install --no-warn-script-location";
        for (const std::string& pkg : packages) args += L" " + FromUtf8(pkg);
        if (RunTool(pyExe.wstring(), args, py, py / L"pip-install.log", &cancel_, true) != 0)
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
        std::wstring cmd = L"\"" + ollamaExe + L"\"";
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        for (int i = 0; i < 40 && !cancel_; ++i) {       // Ollama's tray app starts its server after a moment
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

    Finish(notify, true, L"");
}

} // namespace eflow
