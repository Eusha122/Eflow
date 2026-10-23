#include "services/AudioDevices.h"

#include <initguid.h>

#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propkeydef.h>
#include <propsys.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <functional>
#include <sstream>

#include "core/Utf8.h"
#include "core/Json.h"
#include "core/Log.h"
#include "services/DeviceReadiness.h"

namespace eflow {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring ConfigPath(bool createDirectory) {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\Eflow";
        if (createDirectory) CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\voice.json";
    }
    CoTaskMemFree(base);
    return path;
}

// Tells the worker something about the devices changed (one gets plugged in, removed, a default moves).
class Notifier final : public IMMNotificationClient {
public:
    explicit Notifier(std::function<void()> onChange) : onChange_(std::move(onChange)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { onChange_(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { onChange_(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { onChange_(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { onChange_(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }   // fires constantly

private:
    std::atomic<ULONG> refs_{1};
    std::function<void()> onChange_;
};

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return L"Unknown device";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring name = L"Unknown device";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

std::wstring DeviceId(IMMDevice* device) {
    LPWSTR raw = nullptr;
    std::wstring id;
    if (SUCCEEDED(device->GetId(&raw)) && raw) id = raw;
    CoTaskMemFree(raw);
    return id;
}

// The interface Windows' own Sound settings use to change the default device. Not in the public SDK, but
// unchanged since Windows Vista and used by every sound-switcher tool.
struct DECLSPEC_UUID("F8679F50-850A-41CF-9C72-430F290290C8") IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
class DECLSPEC_UUID("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9") PolicyConfigClient;

bool MakeDefaultDevice(const std::wstring& id) {
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy)))) return false;
    bool ok = true;
    for (ERole role : {eConsole, eMultimedia, eCommunications}) ok &= SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), role));
    return ok;
}

// EndpointFormFactor of a device: speakers, headphones, headset, monitor...
int FormFactorOf(IMMDevice* device) {
    static const PROPERTYKEY kFormFactor = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return 10;
    PROPVARIANT v;
    PropVariantInit(&v);
    int form = 10;
    if (SUCCEEDED(props->GetValue(kFormFactor, &v)) && v.vt == VT_UI4) form = static_cast<int>(v.ulVal);
    PropVariantClear(&v);
    return form;
}

// Apps with an active audio stream on this endpoint right now (not Eflow's own test streams).
std::vector<std::wstring> ActiveSessionNames(IAudioSessionManager2* manager) {
    std::vector<std::wstring> names;
    if (!manager) return names;
    ComPtr<IAudioSessionEnumerator> list;
    if (FAILED(manager->GetSessionEnumerator(&list))) return names;
    int count = 0;
    list->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> control2;
        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(list->GetSession(i, &control)) || FAILED(control->GetState(&state)) || state != AudioSessionStateActive) continue;
        if (FAILED(control.As(&control2)) || control2->IsSystemSoundsSession() == S_OK) continue;
        DWORD pid = 0;
        if (FAILED(control2->GetProcessId(&pid)) || pid == 0 || pid == GetCurrentProcessId()) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        std::wstring name;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            name = path;
            name = name.substr(name.find_last_of(L'\\') + 1);
            if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
            if (!name.empty()) name[0] = static_cast<wchar_t>(std::towupper(name[0]));
        }
        CloseHandle(process);
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
}

// ---- microphone / speaker test --------------------------------------------------------------------------------

struct SampleFormat {
    enum Kind { Unsupported, F32, I16, I24, I32 } kind = Unsupported;
    UINT channels = 0;
    UINT blockAlign = 0;
    UINT rate = 0;
};

SampleFormat Describe(const WAVEFORMATEX* f) {
    SampleFormat out;
    out.channels = f->nChannels;
    out.blockAlign = f->nBlockAlign;
    out.rate = f->nSamplesPerSec;
    WORD tag = f->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) tag = static_cast<WORD>(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat.Data1);
    if (tag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample == 32) out.kind = SampleFormat::F32;
    else if (tag == WAVE_FORMAT_PCM && f->wBitsPerSample == 16) out.kind = SampleFormat::I16;
    else if (tag == WAVE_FORMAT_PCM && f->wBitsPerSample == 24) out.kind = SampleFormat::I24;
    else if (tag == WAVE_FORMAT_PCM && f->wBitsPerSample == 32) out.kind = SampleFormat::I32;
    return out;
}

float PeakOf(const BYTE* data, UINT32 frames, const SampleFormat& f) {
    float peak = 0;
    const size_t samples = static_cast<size_t>(frames) * f.channels;
    for (size_t i = 0; i < samples; ++i) {
        float v = 0;
        switch (f.kind) {
        case SampleFormat::F32: v = reinterpret_cast<const float*>(data)[i]; break;
        case SampleFormat::I16: v = reinterpret_cast<const int16_t*>(data)[i] / 32768.0f; break;
        case SampleFormat::I32: v = reinterpret_cast<const int32_t*>(data)[i] / 2147483648.0f; break;
        case SampleFormat::I24: {
            const BYTE* b = data + i * 3;
            const int32_t raw = (b[0] << 8) | (b[1] << 16) | (static_cast<int32_t>(b[2]) << 24);
            v = raw / 2147483648.0f;
            break;
        }
        default: break;
        }
        peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

void WriteSample(BYTE* frame, const SampleFormat& f, float v) {
    for (UINT c = 0; c < f.channels; ++c) {
        switch (f.kind) {
        case SampleFormat::F32: reinterpret_cast<float*>(frame)[c] = v; break;
        case SampleFormat::I16: reinterpret_cast<int16_t*>(frame)[c] = static_cast<int16_t>(v * 32767.0f); break;
        case SampleFormat::I32: reinterpret_cast<int32_t*>(frame)[c] = static_cast<int32_t>(v * 2147483647.0f); break;
        case SampleFormat::I24: {
            const int32_t raw = static_cast<int32_t>(v * 8388607.0f);
            BYTE* b = frame + c * 3;
            b[0] = static_cast<BYTE>(raw & 0xFF);
            b[1] = static_cast<BYTE>((raw >> 8) & 0xFF);
            b[2] = static_cast<BYTE>((raw >> 16) & 0xFF);
            break;
        }
        default: break;
        }
    }
}

// Two soft bell notes, quiet on purpose (it plays through whatever volume the speaker is set to).
float Chime(double t) {
    auto note = [&](double start, double freq, double length) {
        const double x = t - start;
        if (x < 0 || x > length) return 0.0;
        const double attack = std::min(1.0, x / 0.008);
        return std::sin(2.0 * 3.14159265358979 * freq * x) * attack * std::exp(-5.5 * x);
    };
    return static_cast<float>(0.20 * (note(0.0, 880.0, 0.6) + 0.9 * note(0.17, 1318.5, 0.75)));
}


} // namespace

bool MicrophoneBlockedByPrivacy() {
    for (const wchar_t* key : {L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone",
                               L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone\\NonPackaged"}) {
        wchar_t value[32]{};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"Value", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS && wcscmp(value, L"Deny") == 0)
            return true;
    }
    return false;
}

AudioDevices::AudioDevices(HWND notify, UINT message) : notify_(notify), message_(message) {}

AudioDevices::~AudioDevices() {
    StopTest();
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void AudioDevices::Start() {
    LoadSelection();
    worker_ = std::thread([this] { Run(); });
}

void AudioDevices::Watch(int delta) {
    {
        std::lock_guard lock(mutex_);
        watchers_ = std::max(0, watchers_ + delta);
    }
    wake_.notify_all();
}

AudioSnapshot AudioDevices::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void AudioDevices::Select(bool input, const std::wstring& id) {
    {
        std::lock_guard lock(mutex_);
        const int side = input ? 0 : 1;
        pendingDefault_[side] = id;                          // the worker makes it the Windows default (all roles)
        selId_[side].clear();                                // so "follow the system default" is always right for Solu too
        selName_[side].clear();
        SaveSelectionLocked();
        refresh_ = true;
    }
    wake_.notify_all();
}

void AudioDevices::SetMute(bool input, bool mute) {
    {
        std::lock_guard lock(mutex_);
        mutePending_[input ? 0 : 1] = mute ? 1 : 0;
    }
    wake_.notify_all();
}

void AudioDevices::StartTest(bool input) {
    StopTest();
    std::wstring id;
    bool muted = false;
    {
        std::lock_guard lock(mutex_);
        id = selId_[input ? 0 : 1];
        muted = input ? snapshot_.input.muted : snapshot_.output.muted;
        snapshot_.test = AudioTest{};
        snapshot_.test.state = AudioTest::State::Running;
        snapshot_.test.input = input;
    }
    PostMessageW(notify_, message_, 0, 0);
    testStop_ = false;
    testThread_ = std::thread([this, input, id, muted] { TestRun(input, id, muted); });
}

void AudioDevices::StopTest() {
    testStop_ = true;
    if (testThread_.joinable()) testThread_.join();
}

void AudioDevices::TestRun(bool input, std::wstring id, bool muted) {
    using V = AudioTest::Verdict;
    auto finish = [&](V verdict, bool cancelled = false) {
        {
            std::lock_guard lock(mutex_);
            snapshot_.test.state = cancelled ? AudioTest::State::Idle : AudioTest::State::Done;
            snapshot_.test.verdict = cancelled ? V::None : verdict;
            snapshot_.test.level = 0;
            snapshot_.test.doneAt = GetTickCount64();
        }
        Log("audio: %s test finished, verdict=%d peak=%.4f", input ? "microphone" : "speaker", static_cast<int>(cancelled ? V::None : verdict), snapshot_.test.peak);
        PostMessageW(notify_, message_, 0, 0);
    };
    if (muted) return finish(V::Muted);
    if (input && MicrophoneBlockedByPrivacy()) return finish(V::Blocked);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return finish(V::Error);
    struct Uninit { ~Uninit() { CoUninitialize(); } } uninit;

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return finish(V::Error);
    const HRESULT found = id.empty() ? enumerator->GetDefaultAudioEndpoint(input ? eCapture : eRender, eConsole, &device)
                                     : enumerator->GetDevice(id.c_str(), &device);
    if (FAILED(found) || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return finish(V::Error);
    WAVEFORMATEX* format = nullptr;
    if (FAILED(client->GetMixFormat(&format)) || !format) return finish(V::Error);
    const SampleFormat sf = Describe(format);
    const HRESULT init = sf.kind == SampleFormat::Unsupported ? E_FAIL
                                                              : client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, format, nullptr);
    CoTaskMemFree(format);
    if (init == E_ACCESSDENIED) return finish(V::Blocked);
    if (FAILED(init)) return finish(V::Error);

    float peakMax = 0, shown = 0;
    ULONGLONG lastPost = 0;
    auto publish = [&](float level) {
        shown = std::max(level, shown * 0.80f);                      // fast attack, quick fall: a lively meter
        const ULONGLONG now = GetTickCount64();
        if (now - lastPost < 45) return;
        lastPost = now;
        {
            std::lock_guard lock(mutex_);
            snapshot_.test.level = shown;
            snapshot_.test.peak = peakMax;
        }
        PostMessageW(notify_, message_, 0, 0);
    };

    if (input) {
        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) return finish(V::Error);
        const ULONGLONG start = GetTickCount64();
        while (!testStop_ && GetTickCount64() - start < 5000) {
            Sleep(15);
            float peak = 0;
            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) peak = std::max(peak, PeakOf(data, frames, sf));
                capture->ReleaseBuffer(frames);
            }
            peakMax = std::max(peakMax, peak);
            publish(peak);
        }
        client->Stop();
        if (testStop_) return finish(V::None, true);
        return finish(peakMax >= 0.02f ? V::Works : peakMax >= 0.003f ? V::Quiet : V::Silent);
    }

    ComPtr<IAudioRenderClient> render;
    UINT32 bufferFrames = 0;
    if (FAILED(client->GetService(IID_PPV_ARGS(&render))) || FAILED(client->GetBufferSize(&bufferFrames))) return finish(V::Error);
    const UINT64 total = static_cast<UINT64>(sf.rate * 1.0);
    UINT64 produced = 0;
    auto fill = [&]() -> float {                                        // top the buffer up with the next stretch of chime
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) return 0.0f;
        const UINT32 room = static_cast<UINT32>(std::min<UINT64>(bufferFrames - padding, total - produced));   // (at most the buffer)
        if (!room) return 0.0f;
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(room, &data))) return 0.0f;
        float peak = 0;
        for (UINT32 i = 0; i < room; ++i) {
            const float v = Chime(static_cast<double>(produced + i) / sf.rate);
            WriteSample(data + static_cast<size_t>(i) * sf.blockAlign, sf, v);
            peak = std::max(peak, std::fabs(v));
        }
        render->ReleaseBuffer(room, 0);
        produced += room;
        return peak / 0.2f;
    };
    fill();
    if (FAILED(client->Start())) return finish(V::Error);
    const ULONGLONG start = GetTickCount64();
    while (!testStop_ && GetTickCount64() - start < 2500) {
        Sleep(12);
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        const float level = produced < total ? fill() : std::min(1.0f, static_cast<float>(padding) / (sf.rate * 0.12f));
        peakMax = std::max(peakMax, level);
        publish(level);
        if (produced >= total && padding == 0) break;
    }
    client->Stop();
    if (testStop_) return finish(V::None, true);
    finish(V::Works);
}

// ---- persistence: {"input": {"id": "...", "name": "..."}, "output": {...}} -------------------------

void AudioDevices::LoadSelection() {
    const std::wstring path = ConfigPath(false);
    if (path.empty()) return;
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    std::stringstream text;
    text << file.rdbuf();
    json::Value root;
    std::string error;
    if (!json::Parse(text.str(), root, error) || root.type != json::Value::Type::Object) return;
    std::lock_guard lock(mutex_);
    const char* keys[2] = {"input", "output"};
    for (int side = 0; side < 2; ++side) {
        json::Value* entry = root.Find(keys[side]);
        if (!entry || entry->type != json::Value::Type::Object) continue;
        if (json::Value* id = entry->Find("id"); id && id->type == json::Value::Type::String) selId_[side] = FromUtf8(id->text);
        if (json::Value* name = entry->Find("name"); name && name->type == json::Value::Type::String) selName_[side] = FromUtf8(name->text);
    }
}

void AudioDevices::SaveSelectionLocked() const {
    const std::wstring path = ConfigPath(true);
    if (path.empty()) return;
    json::Value root = json::Value::Object();
    const char* keys[2] = {"input", "output"};
    for (int side = 0; side < 2; ++side) {
        json::Value entry = json::Value::Object();
        entry.Member("id") = json::Value::String(ToUtf8(selId_[side]));
        entry.Member("name") = json::Value::String(ToUtf8(selName_[side]));
        root.Member(keys[side]) = std::move(entry);
    }
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << json::Write(root);
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);   // Solu never sees a half-written file
}

// ---- worker ----------------------------------------------------------------------------------

void AudioDevices::Run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        Log("audio: device enumerator unavailable; voice device list disabled");
        CoUninitialize();
        return;
    }
    Notifier* notifier = new Notifier([this] {
        {
            std::lock_guard lock(mutex_);
            refresh_ = true;
        }
        wake_.notify_all();
    });
    enumerator->RegisterEndpointNotificationCallback(notifier);

    ComPtr<IAudioMeterInformation> meter[2];
    ComPtr<IAudioEndpointVolume> volume[2];
    ComPtr<IAudioSessionManager2> sessions[2];
    int tick = 0;
    std::wstring meterId[2];
    const EDataFlow flows[2] = {eCapture, eRender};

    for (;;) {
        bool doRefresh = false, watching = false;
        int muteNow[2] = {-1, -1};
        std::wstring makeDefault[2];
        {
            std::unique_lock lock(mutex_);
            // Event-driven; the short timeout only exists while the island is open and wants live levels.
            wake_.wait_for(lock, watchers_ > 0 ? std::chrono::milliseconds(66) : std::chrono::seconds(60),
                           [&] { return stop_ || refresh_ || mutePending_[0] >= 0 || mutePending_[1] >= 0 || !pendingDefault_[0].empty() || !pendingDefault_[1].empty(); });
            if (stop_) break;
            doRefresh = refresh_;
            refresh_ = false;
            watching = watchers_ > 0;
            for (int side = 0; side < 2; ++side) {
                muteNow[side] = mutePending_[side];
                mutePending_[side] = -1;
                makeDefault[side] = std::move(pendingDefault_[side]);
                pendingDefault_[side].clear();
            }
        }
        for (int side = 0; side < 2; ++side)
            if (!makeDefault[side].empty())
                Log("audio: make %s the Windows default: %s", side == 0 ? "microphone" : "speaker", MakeDefaultDevice(makeDefault[side]) ? "ok" : "FAILED");
        for (int side = 0; side < 2; ++side)                         // a mute requested from the island
            if (muteNow[side] >= 0 && volume[side]) volume[side]->SetMute(muteNow[side] ? TRUE : FALSE, nullptr);

        bool changed = false;
        if (doRefresh) {
            AudioSnapshot fresh;
            std::wstring wanted[2], lastName[2];
            {
                std::lock_guard lock(mutex_);
                for (int side = 0; side < 2; ++side) {
                    wanted[side] = selId_[side];
                    lastName[side] = selName_[side];
                }
            }
            for (int side = 0; side < 2; ++side) {
                AudioSide& out = side == 0 ? fresh.input : fresh.output;
                std::wstring defaultId, defaultName;
                ComPtr<IMMDevice> def;
                if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flows[side], eConsole, &def))) {
                    defaultId = DeviceId(def.Get());
                    defaultName = FriendlyName(def.Get());
                }
                ComPtr<IMMDeviceCollection> list;
                if (SUCCEEDED(enumerator->EnumAudioEndpoints(flows[side], DEVICE_STATE_ACTIVE, &list))) {
                    UINT count = 0;
                    list->GetCount(&count);
                    for (UINT i = 0; i < count; ++i) {
                        ComPtr<IMMDevice> device;
                        if (FAILED(list->Item(i, &device))) continue;
                        AudioDevice d{DeviceId(device.Get()), FriendlyName(device.Get()), false};
                        d.isDefault = d.id == defaultId;
                        const Readiness r = CheckReadiness(d.name);
                        d.ready = r.ready;
                        d.hint = r.hint;
                        d.form = FormFactorOf(device.Get());
                        out.devices.push_back(std::move(d));
                    }
                }
                out.selectedId = wanted[side];
                if (wanted[side].empty()) {
                    out.connected = !defaultId.empty();
                    out.name = out.connected ? defaultName : L"No device found";
                    for (const auto& d : out.devices)
                        if (d.isDefault) out.ready = d.ready, out.hint = d.hint;
                } else {
                    const auto it = std::find_if(out.devices.begin(), out.devices.end(),
                                                 [&](const AudioDevice& d) { return d.id == wanted[side]; });
                    out.connected = it != out.devices.end();
                    out.name = out.connected ? it->name : (lastName[side].empty() ? L"Selected device" : lastName[side]);
                    if (out.connected) out.ready = it->ready, out.hint = it->hint;
                    if (out.connected) {
                        std::lock_guard lock(mutex_);
                        selName_[side] = it->name;         // remember it for when it is unplugged
                    }
                }
                const std::wstring meterTarget = !out.connected ? L"" : (wanted[side].empty() ? defaultId : wanted[side]);
                if (meterTarget != meterId[side]) {        // point the level meter at the device in use
                    meter[side].Reset();
                    volume[side].Reset();
                    sessions[side].Reset();
                    meterId[side] = meterTarget;
                    if (!meterTarget.empty()) {
                        ComPtr<IMMDevice> device;
                        if (SUCCEEDED(enumerator->GetDevice(meterTarget.c_str(), &device)))
                        {
                            device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &meter[side]);
                            device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume[side]);
                            device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &sessions[side]);
                        }
                    }
                }
            }
            fresh.valid = true;
            {
                std::lock_guard lock(mutex_);
                fresh.input.level = snapshot_.input.level;
                fresh.output.level = snapshot_.output.level;
                fresh.input.muted = snapshot_.input.muted;
                fresh.output.muted = snapshot_.output.muted;
                fresh.input.inUseBy = snapshot_.input.inUseBy;
                fresh.output.inUseBy = snapshot_.output.inUseBy;
                fresh.test = snapshot_.test;
                snapshot_ = std::move(fresh);
            }
            Log("audio: %zu microphone(s), %zu output(s)", Snapshot().input.devices.size(), Snapshot().output.devices.size());
            changed = true;
        }

        if (doRefresh || watching || muteNow[0] >= 0 || muteNow[1] >= 0) {   // read the mute state back (a hardware key may flip it)
            BOOL muted[2] = {FALSE, FALSE};
            for (int side = 0; side < 2; ++side)
                if (volume[side]) volume[side]->GetMute(&muted[side]);
            std::lock_guard lock(mutex_);
            if (snapshot_.input.muted != (muted[0] != FALSE) || snapshot_.output.muted != (muted[1] != FALSE)) {
                snapshot_.input.muted = muted[0] != FALSE;
                snapshot_.output.muted = muted[1] != FALSE;
                changed = true;
            }
        }
        if (watching) {
            ++tick;
            if (tick % 8 == 0 || doRefresh) {                          // ~every half second: apps using each device
                std::vector<std::wstring> users[2] = {ActiveSessionNames(sessions[0].Get()), ActiveSessionNames(sessions[1].Get())};
                std::lock_guard lock(mutex_);
                if (snapshot_.input.inUseBy != users[0] || snapshot_.output.inUseBy != users[1]) {
                    snapshot_.input.inUseBy = std::move(users[0]);
                    snapshot_.output.inUseBy = std::move(users[1]);
                    changed = true;
                }
            }
            if (tick % 8 == 0 || doRefresh) {                          // which app plays on which output (the Music tab shows it)
                std::vector<std::wstring> ids;
                {
                    std::lock_guard lock(mutex_);
                    for (const auto& d : snapshot_.output.devices) ids.push_back(d.id);
                }
                std::vector<std::vector<std::wstring>> apps;
                for (const std::wstring& id : ids) {
                    ComPtr<IMMDevice> device;
                    ComPtr<IAudioSessionManager2> manager;
                    if (SUCCEEDED(enumerator->GetDevice(id.c_str(), &device)))
                        device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager);
                    apps.push_back(ActiveSessionNames(manager.Get()));
                }
                std::lock_guard lock(mutex_);
                for (size_t i = 0; i < apps.size() && i < snapshot_.output.devices.size(); ++i)
                    if (snapshot_.output.devices[i].id == ids[i] && snapshot_.output.devices[i].apps != apps[i]) {
                        snapshot_.output.devices[i].apps = std::move(apps[i]);
                        changed = true;
                    }
            }
            if (tick % 15 == 0) {                                      // ~every second: is the phone behind a virtual device connected?
                AudioSnapshot copy = Snapshot();
                bool differs = false;
                for (AudioSide* side : {&copy.input, &copy.output}) {
                    for (auto& d : side->devices) {
                        const Readiness r = CheckReadiness(d.name);
                        if (r.ready != d.ready || r.hint != d.hint) differs = true;
                        d.ready = r.ready;
                        d.hint = r.hint;
                    }
                    side->ready = false;
                    side->hint.clear();
                    for (const auto& d : side->devices)
                        if (side->selectedId.empty() ? d.isDefault : d.id == side->selectedId) side->ready = d.ready, side->hint = d.hint;
                }
                if (differs) {
                    std::lock_guard lock(mutex_);
                    snapshot_.input.devices = copy.input.devices;
                    snapshot_.output.devices = copy.output.devices;
                    snapshot_.input.ready = copy.input.ready;
                    snapshot_.input.hint = copy.input.hint;
                    snapshot_.output.ready = copy.output.ready;
                    snapshot_.output.hint = copy.output.hint;
                    changed = true;
                }
            }
            float levels[2] = {0, 0};
            for (int side = 0; side < 2; ++side) {
                float peak = 0;
                if (meter[side] && FAILED(meter[side]->GetPeakValue(&peak))) peak = 0;
                levels[side] = peak;
            }
            std::lock_guard lock(mutex_);
            if (std::fabs(levels[0] - snapshot_.input.level) > 0.004f || std::fabs(levels[1] - snapshot_.output.level) > 0.004f) {
                snapshot_.input.level = levels[0];
                snapshot_.output.level = levels[1];
                changed = true;
            }
        }
        if (changed) PostMessageW(notify_, message_, 0, 0);
    }

    enumerator->UnregisterEndpointNotificationCallback(notifier);
    notifier->Release();
    meter[0].Reset();
    meter[1].Reset();
    volume[0].Reset();
    volume[1].Reset();
    sessions[0].Reset();
    sessions[1].Reset();
    CoUninitialize();
}

} // namespace eflow
