#include "services/SystemMonitor.h"

#include <winsock2.h>
#include <ws2ipdef.h>

#include <comdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/Log.h"

namespace eflow {
namespace {

using Microsoft::WRL::ComPtr;

// ---- PDH (Windows performance counters, no admin needed) -------------------------------------

class Pdh {
public:
    ~Pdh() {
        if (query_) PdhCloseQuery(query_);
    }
    bool Open() { return PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS; }
    PDH_HCOUNTER Add(const wchar_t* path) {
        PDH_HCOUNTER c = nullptr;
        return PdhAddEnglishCounterW(query_, path, 0, &c) == ERROR_SUCCESS ? c : nullptr;
    }
    void Collect() { PdhCollectQueryData(query_); }
    bool Value(PDH_HCOUNTER c, double& out) const {
        PDH_FMT_COUNTERVALUE v{};
        if (!c || PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS ||
            (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA))
            return false;
        out = v.doubleValue;
        return true;
    }
    // Wildcard counters: every matching instance with its value.
    std::vector<std::pair<std::wstring, double>> Instances(PDH_HCOUNTER c) const {
        std::vector<std::pair<std::wstring, double>> out;
        DWORD size = 0, count = 0;
        if (!c || PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA) return out;
        std::vector<char> buffer(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return out;
        for (DWORD i = 0; i < count; ++i)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                out.emplace_back(items[i].szName, items[i].FmtValue.doubleValue);
        return out;
    }

private:
    PDH_HQUERY query_ = nullptr;
};

// ---- NVIDIA (NVML ships with the display driver) -----------------------------------------------

class Nvml {
public:
    struct Reading {
        std::wstring name;
        float usage = 0, tempC = kNoValue, fanPercent = kNoValue, powerW = kNoValue, clockMHz = kNoValue;
        uint64_t vramUsed = 0, vramTotal = 0;
    };

    ~Nvml() {
        if (shutdown_) shutdown_();
        if (lib_) FreeLibrary(lib_);
    }
    bool Init() {
        lib_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);   // never from the app folder
        if (!lib_) return false;
        auto init = Fn<int (*)()>("nvmlInit_v2");
        shutdown_ = Fn<int (*)()>("nvmlShutdown");
        getHandle_ = Fn<int (*)(unsigned, void**)>("nvmlDeviceGetHandleByIndex_v2");
        getCount_ = Fn<int (*)(unsigned*)>("nvmlDeviceGetCount_v2");
        getName_ = Fn<int (*)(void*, char*, unsigned)>("nvmlDeviceGetName");
        getUtil_ = Fn<int (*)(void*, Utilization*)>("nvmlDeviceGetUtilizationRates");
        getTemp_ = Fn<int (*)(void*, int, unsigned*)>("nvmlDeviceGetTemperature");
        getFan_ = Fn<int (*)(void*, unsigned*)>("nvmlDeviceGetFanSpeed");
        getMem_ = Fn<int (*)(void*, Memory*)>("nvmlDeviceGetMemoryInfo");
        getPower_ = Fn<int (*)(void*, unsigned*)>("nvmlDeviceGetPowerUsage");
        getClock_ = Fn<int (*)(void*, int, unsigned*)>("nvmlDeviceGetClockInfo");
        if (!init || !shutdown_ || !getHandle_ || !getCount_ || !getUtil_ || init() != 0) {
            shutdown_ = nullptr;
            return false;
        }
        unsigned count = 0;
        if (getCount_(&count) != 0 || count == 0 || getHandle_(0, &device_) != 0) return false;
        char name[96]{};
        if (getName_ && getName_(device_, name, sizeof(name)) == 0) name_ = std::wstring(name, name + strlen(name));
        return true;
    }
    bool Read(Reading& r) const {
        Utilization u{};
        if (!device_ || getUtil_(device_, &u) != 0) return false;
        r.name = name_;
        r.usage = static_cast<float>(u.gpu);
        unsigned v = 0;
        if (getTemp_ && getTemp_(device_, 0 /* GPU die */, &v) == 0) r.tempC = static_cast<float>(v);
        if (getFan_ && getFan_(device_, &v) == 0) r.fanPercent = static_cast<float>(v);
        if (getPower_ && getPower_(device_, &v) == 0) r.powerW = v / 1000.0f;
        if (getClock_ && getClock_(device_, 0 /* graphics */, &v) == 0) r.clockMHz = static_cast<float>(v);
        Memory m{};
        if (getMem_ && getMem_(device_, &m) == 0) {
            r.vramTotal = m.total;
            r.vramUsed = m.used;
        }
        return true;
    }

private:
    struct Utilization { unsigned gpu, memory; };
    struct Memory { unsigned long long total, free, used; };
    template <class F> F Fn(const char* name) const { return reinterpret_cast<F>(GetProcAddress(lib_, name)); }

    HMODULE lib_ = nullptr;
    void* device_ = nullptr;
    std::wstring name_;
    int (*shutdown_)() = nullptr;
    int (*getHandle_)(unsigned, void**) = nullptr;
    int (*getCount_)(unsigned*) = nullptr;
    int (*getName_)(void*, char*, unsigned) = nullptr;
    int (*getUtil_)(void*, Utilization*) = nullptr;
    int (*getTemp_)(void*, int, unsigned*) = nullptr;
    int (*getFan_)(void*, unsigned*) = nullptr;
    int (*getMem_)(void*, Memory*) = nullptr;
    int (*getPower_)(void*, unsigned*) = nullptr;
    int (*getClock_)(void*, int, unsigned*) = nullptr;
};

// ---- optional sensor bridge: LibreHardwareMonitor / OpenHardwareMonitor via WMI ------------------
// Windows exposes no CPU temperature or case-fan speed to normal programs; those need a hardware
// driver and administrator rights, which is what these tools provide. If one is running, its sensors
// are published through WMI and we read them from there. Eflow never loads a driver itself.

class SensorBridge {
public:
    struct Reading {
        float cpuTempC = kNoValue;
        std::vector<FanReading> fans;
    };

    // Returns false while no monitoring tool is running (cheap: the WMI namespace simply doesn't exist).
    bool Read(Reading& out) {
        ComPtr<IWbemServices> services = Connect();
        if (!services) return false;
        ComPtr<IEnumWbemClassObject> rows;
        if (FAILED(services->ExecQuery(bstr_t(L"WQL"),
                                       bstr_t(L"SELECT Identifier, Name, SensorType, Value FROM Sensor "
                                              L"WHERE SensorType = 'Temperature' OR SensorType = 'Fan'"),
                                       WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows))) {
            services_.Reset();                              // the tool was closed: look again later
            return false;
        }
        float best = kNoValue, bestScore = -1;
        for (;;) {
            ComPtr<IWbemClassObject> row;
            ULONG got = 0;
            if (FAILED(rows->Next(5000, 1, &row, &got)) || got == 0) break;
            const std::wstring id = Lower(Text(row.Get(), L"Identifier")), name = Text(row.Get(), L"Name"),
                               type = Text(row.Get(), L"SensorType");
            VARIANT v;
            VariantInit(&v);
            const bool ok = SUCCEEDED(row->Get(L"Value", 0, &v, nullptr, nullptr)) && v.vt == VT_R4;
            const float value = ok ? v.fltVal : kNoValue;
            VariantClear(&v);
            if (!Has(value)) continue;
            if (type == L"Fan") {
                out.fans.push_back({name, static_cast<int>(value + 0.5f)});
            } else if (type == L"Temperature" && id.find(L"cpu") != std::wstring::npos && value > 0 && value < 150) {
                float score = 1;                            // prefer the whole-package reading over single cores
                const std::wstring n = Lower(name);
                if (n.find(L"package") != std::wstring::npos || n.find(L"tctl") != std::wstring::npos ||
                    n.find(L"tdie") != std::wstring::npos || n.find(L"cpu") != std::wstring::npos)
                    score = 3;
                else if (n.find(L"max") != std::wstring::npos)
                    score = 2;
                if (score > bestScore || (score == bestScore && value > best)) {
                    best = value;
                    bestScore = score;
                }
            }
        }
        out.cpuTempC = best;
        return true;
    }

private:
    static std::wstring Lower(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return s;
    }
    static std::wstring Text(IWbemClassObject* o, const wchar_t* prop) {
        VARIANT v;
        VariantInit(&v);
        std::wstring s;
        if (SUCCEEDED(o->Get(prop, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal) s = v.bstrVal;
        VariantClear(&v);
        return s;
    }
    ComPtr<IWbemServices> Connect() {
        if (services_) return services_;
        if (GetTickCount64() < retryAt_) return nullptr;
        retryAt_ = GetTickCount64() + 10'000;               // look for the tool every 10 s at most
        if (!locator_ && FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator_))))
            return nullptr;
        for (const wchar_t* ns : {L"ROOT\\LibreHardwareMonitor", L"ROOT\\OpenHardwareMonitor"}) {
            ComPtr<IWbemServices> s;
            if (SUCCEEDED(locator_->ConnectServer(bstr_t(ns), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &s)) &&
                SUCCEEDED(CoSetProxyBlanket(s.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) {
                services_ = s;
                Log("system: sensor bridge found (%ls)", ns);
                return services_;
            }
        }
        return nullptr;
    }

    ComPtr<IWbemLocator> locator_;
    ComPtr<IWbemServices> services_;
    ULONGLONG retryAt_ = 0;
};

} // namespace

// ---- the sampler: owns every data source, lives on the worker thread ---------------------------

struct SystemMonitor::Sampler {
    Pdh pdh;
    PDH_HCOUNTER cpuUtility = nullptr, cpuPerformance = nullptr, cpuFrequency = nullptr, gpuEngines = nullptr;
    ULARGE_INTEGER lastIdle{}, lastKernel{}, lastUser{};
    Nvml nvml;
    bool nvmlOk = false;
    uint64_t lastIn = 0, lastOut = 0;
    ULONGLONG lastNetTick = 0;
    SensorBridge bridge;
    std::wstring cpuName;
    int cores = 0, threads = 0;

    Sampler() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        threads = static_cast<int>(si.dwNumberOfProcessors);
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        std::vector<char> buf(len);
        if (len && GetLogicalProcessorInformationEx(
                       RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len)) {
            for (DWORD off = 0; off < len;) {
                ++cores;
                off += reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off)->Size;
            }
        }
        wchar_t name[128]{};
        DWORD size = sizeof(name);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                         RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) {
            cpuName = name;
            cpuName.erase(0, cpuName.find_first_not_of(L' '));
            for (const wchar_t* junk : {L" with Radeon Graphics", L" with Radeon Vega Graphics", L"(R)", L"(TM)", L" CPU"})
                for (size_t at; (at = cpuName.find(junk)) != std::wstring::npos;) cpuName.erase(at, wcslen(junk));
        }
        if (pdh.Open()) {
            cpuUtility = pdh.Add(L"\\Processor Information(_Total)\\% Processor Utility");
            cpuPerformance = pdh.Add(L"\\Processor Information(_Total)\\% Processor Performance");
            cpuFrequency = pdh.Add(L"\\Processor Information(_Total)\\Processor Frequency");
        }
        nvmlOk = nvml.Init();
        if (!nvmlOk && pdh.Open())                          // any other GPU: utilisation from the GPU engine counters
            gpuEngines = pdh.Add(L"\\GPU Engine(*)\\Utilization Percentage");
        Log("system: cpu=%ls (%d cores / %d threads), gpu via %s", cpuName.c_str(), cores, threads,
            nvmlOk ? "NVML" : gpuEngines ? "performance counters" : "none");
        Baseline();
    }

    // Takes the starting point for every delta-based reading.
    void Baseline() {
        pdh.Collect();
        FILETIME idle, kernel, user;
        GetSystemTimes(&idle, &kernel, &user);
        lastIdle.QuadPart = (static_cast<ULONGLONG>(idle.dwHighDateTime) << 32) | idle.dwLowDateTime;
        lastKernel.QuadPart = (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        lastUser.QuadPart = (static_cast<ULONGLONG>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
        NetTotals(lastIn, lastOut);
        lastNetTick = GetTickCount64();
    }

    // Hardware adapters only: virtual, tunnel and loopback interfaces would count the same traffic twice.
    static void NetTotals(uint64_t& in, uint64_t& out) {
        in = out = 0;
        MIB_IF_TABLE2* table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR) return;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL ||
                !row.InterfaceAndOperStatusFlags.HardwareInterface)
                continue;
            in += row.InOctets;
            out += row.OutOctets;
        }
        FreeMibTable(table);
    }

    void Sample(SystemSnapshot& s) {
        pdh.Collect();

        // CPU: "Processor Utility" is what Task Manager shows; fall back to kernel times.
        double v = 0;
        FILETIME idle, kernel, user;
        GetSystemTimes(&idle, &kernel, &user);
        ULARGE_INTEGER i{}, k{}, u{};
        i.LowPart = idle.dwLowDateTime, i.HighPart = idle.dwHighDateTime;
        k.LowPart = kernel.dwLowDateTime, k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime, u.HighPart = user.dwHighDateTime;
        const double total = static_cast<double>((k.QuadPart - lastKernel.QuadPart) + (u.QuadPart - lastUser.QuadPart));
        const double busy = total - static_cast<double>(i.QuadPart - lastIdle.QuadPart);
        lastIdle = i, lastKernel = k, lastUser = u;
        s.cpuUsage = pdh.Value(cpuUtility, v) ? static_cast<float>(v) : (total > 0 ? static_cast<float>(100.0 * busy / total) : 0.0f);
        s.cpuUsage = std::clamp(s.cpuUsage, 0.0f, 100.0f);
        double perf = 0, base = 0;
        if (pdh.Value(cpuPerformance, perf) && pdh.Value(cpuFrequency, base) && base > 0)
            s.cpuClockGHz = static_cast<float>(base * perf / 100.0 / 1000.0);
        s.cpuName = cpuName;
        s.cpuCores = cores;
        s.cpuThreads = threads;
        s.cpuHistory.Push(s.cpuUsage);

        // Memory.
        MEMORYSTATUSEX m{sizeof(m)};
        if (GlobalMemoryStatusEx(&m)) {
            s.ramTotal = m.ullTotalPhys;
            s.ramUsed = m.ullTotalPhys - m.ullAvailPhys;
            s.ramUsage = m.ullTotalPhys ? static_cast<float>(100.0 * static_cast<double>(s.ramUsed) / static_cast<double>(m.ullTotalPhys)) : 0.0f;
        }
        s.ramHistory.Push(s.ramUsage);

        // GPU.
        Nvml::Reading g;
        if (nvmlOk && nvml.Read(g)) {
            s.gpuAvailable = true;
            s.gpuName = g.name;
            s.gpuUsage = g.usage;
            s.gpuTempC = g.tempC;
            s.gpuFanPercent = g.fanPercent;
            s.gpuPowerW = g.powerW;
            s.gpuClockMHz = g.clockMHz;
            s.vramUsed = g.vramUsed;
            s.vramTotal = g.vramTotal;
        } else if (gpuEngines) {
            // Busiest engine type across all processes, like Task Manager's per-engine view.
            std::vector<std::pair<std::wstring, double>> byType;
            for (const auto& [name, value] : pdh.Instances(gpuEngines)) {
                const size_t at = name.rfind(L"engtype_");
                const std::wstring type = at == std::wstring::npos ? L"?" : name.substr(at);
                auto it = std::find_if(byType.begin(), byType.end(), [&](auto& e) { return e.first == type; });
                if (it == byType.end()) byType.emplace_back(type, value);
                else it->second += value;
            }
            double best = 0;
            for (const auto& e : byType) best = std::max(best, e.second);
            s.gpuAvailable = !byType.empty();
            s.gpuUsage = static_cast<float>(std::min(best, 100.0));
            s.gpuName = L"Graphics";
        }
        s.gpuHistory.Push(s.gpuUsage);

        // Network.
        uint64_t in = 0, out = 0;
        NetTotals(in, out);
        const ULONGLONG now = GetTickCount64();
        const double dt = std::max(0.2, (now - lastNetTick) / 1000.0);
        s.downBps = in >= lastIn ? 8.0 * static_cast<double>(in - lastIn) / dt : 0;
        s.upBps = out >= lastOut ? 8.0 * static_cast<double>(out - lastOut) / dt : 0;
        lastIn = in, lastOut = out, lastNetTick = now;
        s.downHistory.Push(static_cast<float>(s.downBps));
        s.upHistory.Push(static_cast<float>(s.upBps));

        // Optional sensors.
        SensorBridge::Reading r;
        s.sensorBridge = bridge.Read(r);
        if (s.sensorBridge) {
            s.cpuTempC = r.cpuTempC;
            s.fans = std::move(r.fans);
        } else {
            s.cpuTempC = kNoValue;
            s.fans.clear();
        }
        s.valid = true;
        s.stamp = now;
    }
};

// ---- the monitor thread ------------------------------------------------------------------------

SystemMonitor::SystemMonitor(HWND notify, UINT message) : notify_(notify), message_(message) {}

SystemMonitor::~SystemMonitor() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SystemMonitor::Start() { worker_ = std::thread([this] { Run(); }); }

void SystemMonitor::Watch(int delta) {
    {
        std::lock_guard lock(mutex_);
        watchers_ = std::max(0, watchers_ + delta);
    }
    wake_.notify_all();
}

SystemSnapshot SystemMonitor::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void SystemMonitor::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Sampler sampler;
        SystemSnapshot shown;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || watchers_ > 0; });   // nobody looking: sleep, sample nothing
                if (stop_) break;
            }
            // Someone just started watching: fresh baseline and graphs, first reading after a short interval.
            sampler.Baseline();
            shown = SystemSnapshot{};
            {
                std::unique_lock lock(mutex_);
                if (wake_.wait_for(lock, std::chrono::milliseconds(350), [&] { return stop_ || watchers_ == 0; })) {
                    if (stop_) break;
                    continue;
                }
            }
            for (;;) {
                sampler.Sample(shown);
                {
                    std::lock_guard lock(mutex_);
                    snapshot_ = shown;
                }
                PostMessageW(notify_, message_, 0, 0);
                std::unique_lock lock(mutex_);
                if (wake_.wait_for(lock, std::chrono::seconds(1), [&] { return stop_ || watchers_ == 0; })) break;
            }
            std::lock_guard lock(mutex_);
            if (stop_) break;
        }
    }
    CoUninitialize();
}

} // namespace eflow
