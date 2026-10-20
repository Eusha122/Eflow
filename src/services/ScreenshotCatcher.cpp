#include "services/ScreenshotCatcher.h"

#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/Log.h"

namespace eflow {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

constexpr ULONGLONG kSameShotMs = 5000;          // the clipboard and the folder report one Snipping Tool shot ~1 s apart
constexpr uint32_t kThumbSide = 160;
constexpr size_t kRecentShots = 16;

// Screenshot tools: an image on the clipboard from one of these is a screenshot (from anything else it is just a copied picture).
const wchar_t* const kShotTools[] = {L"snippingtool.exe", L"screenclippinghost.exe", L"screensketch.exe", L"sharex.exe",
                                     L"greenshot.exe", L"lightshot.exe", L"picpick.exe", L"flameshot.exe", L"snagit32.exe",
                                     L"snagiteditor.exe", L"explorer.exe"};

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring CapturesDir() {
    const std::wstring dir = KnownFolder(FOLDERID_LocalAppData) + L"\\Eflow\\Captures";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::wstring ProcessName(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return pid ? L"eflow" : L"";
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &size)) name = fs::path(buf).filename().wstring();
        CloseHandle(h);
    }
    for (wchar_t& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name;
}

// A clipboard DIB (24 or 32 bits, top-down or bottom-up) as PNG bytes, through WIC.
std::vector<uint8_t> DibToPng(const BITMAPINFOHEADER* bih, size_t total) {
    if (bih->biCompression != BI_RGB && bih->biCompression != BI_BITFIELDS) return {};
    if (bih->biBitCount != 32 && bih->biBitCount != 24) return {};
    const int width = bih->biWidth, height = std::abs(bih->biHeight);
    if (width <= 0 || height <= 0) return {};
    const size_t stride = ((static_cast<size_t>(width) * bih->biBitCount + 31) / 32) * 4;
    const size_t masks = bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
    const uint8_t* bits = reinterpret_cast<const uint8_t*>(bih) + bih->biSize + masks;
    if (bih->biSize + masks + stride * height > total) return {};
    std::vector<uint8_t> rows(stride * height);
    for (int y = 0; y < height; ++y)                                       // bottom-up DIBs are stored upside down
        memcpy(&rows[y * stride], bits + (bih->biHeight > 0 ? (height - 1 - y) : y) * stride, stride);
    if (bih->biBitCount == 32)                                             // a screenshot's alpha byte is usually 0: opaque
        for (size_t i = 3; i < rows.size(); i += 4) rows[i] = 255;

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    const GUID format = bih->biBitCount == 32 ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmapFromMemory(width, height, format, static_cast<UINT>(stride), static_cast<UINT>(rows.size()), rows.data(), &bitmap)) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &mem))) return {};
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    std::vector<uint8_t> png(static_cast<size_t>(stat.cbSize.QuadPart));
    if (const void* p = GlobalLock(mem)) {
        memcpy(png.data(), p, png.size());
        GlobalUnlock(mem);
    }
    return png;
}

uint64_t Fingerprint(const std::vector<uint8_t>& bytes) {              // FNV-1a
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : bytes) h = (h ^ b) * 1099511628211ull;
    return h;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// The newest PNG in `folder` created in the last few seconds (a screenshot tool just saved it), or empty.
std::wstring FreshShot(const std::wstring& folder) {
    std::error_code ec;
    std::wstring best;
    fs::file_time_type bestTime{};
    const auto now = fs::file_time_type::clock::now();
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != L".png") continue;
        const auto t = entry.last_write_time(ec);
        if (ec || now - t > std::chrono::milliseconds(kSameShotMs)) continue;
        if (best.empty() || t > bestTime) best = entry.path().wstring(), bestTime = t;
    }
    return best;
}

}  // namespace

ScreenshotCatcher::ScreenshotCatcher(HWND notify, UINT message) : notify_(notify), message_(message) {
    folder_ = KnownFolder(FOLDERID_Screenshots);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    clipboard_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    lastSequence_ = GetClipboardSequenceNumber();                        // what is on the clipboard already is not new
    worker_ = std::thread([this] { Run(); });
}

ScreenshotCatcher::~ScreenshotCatcher() {
    SetEvent(stop_);
    if (worker_.joinable()) worker_.join();
    CloseHandle(stop_);
    CloseHandle(clipboard_);
}

void ScreenshotCatcher::OnClipboardUpdate() { SetEvent(clipboard_); }

void ScreenshotCatcher::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);                       // (WIC)
    HANDLE folder = folder_.empty() ? INVALID_HANDLE_VALUE
                                    : FindFirstChangeNotificationW(folder_.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
    for (;;) {
        HANDLE waits[3] = {stop_, clipboard_, folder};
        const DWORD n = folder == INVALID_HANDLE_VALUE ? 2 : 3;
        const DWORD hit = WaitForMultipleObjects(n, waits, FALSE, INFINITE);
        if (hit == WAIT_OBJECT_0) break;
        if (hit == WAIT_OBJECT_0 + 1) {
            Sleep(250);                                                   // let the tool finish putting every format on it
            TakeClipboard();
        } else if (hit == WAIT_OBJECT_0 + 2) {
            Sleep(400);                                                   // the file is being written
            TakeFolder();
            FindNextChangeNotification(folder);
        }
    }
    if (folder != INVALID_HANDLE_VALUE) FindCloseChangeNotification(folder);
    CoUninitialize();
}

void ScreenshotCatcher::TakeClipboard() {
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == lastSequence_) return;
    lastSequence_ = sequence;
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (!IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(pngFormat)) return;
    const HWND owner = GetClipboardOwner();
    const std::wstring tool = owner ? ProcessName(owner) : L"";               // no owner: the old Print Screen key
    if (tool == L"eflow" || (owner && std::find(std::begin(kShotTools), std::end(kShotTools), tool) == std::end(kShotTools))) return;
    std::vector<uint8_t> png;
    for (int attempt = 0; attempt < 5 && !OpenClipboard(nullptr); ++attempt) Sleep(60);
    if (HANDLE h = IsClipboardFormatAvailable(pngFormat) ? GetClipboardData(pngFormat) : nullptr) {
        if (const auto* p = static_cast<const uint8_t*>(GlobalLock(h))) {
            png.assign(p, p + GlobalSize(h));
            GlobalUnlock(h);
        }
    } else if (HANDLE dib = GetClipboardData(CF_DIB)) {
        if (const auto* bih = static_cast<const BITMAPINFOHEADER*>(GlobalLock(dib))) {
            png = DibToPng(bih, GlobalSize(dib));
            GlobalUnlock(dib);
        }
    }
    CloseClipboard();
    if (png.empty()) return;
    const uint64_t print = Fingerprint(png);
    if (std::find(recent_.begin(), recent_.end(), print) != recent_.end()) return;   // the same picture again: not a new shot
    recent_.push_back(print);
    if (recent_.size() > kRecentShots) recent_.pop_front();
    Sleep(700);                                                           // Snipping Tool also saves it: use that file
    std::wstring path = folder_.empty() ? L"" : FreshShot(folder_);
    if (path.empty()) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf_s(name, L"\\Screenshot %04d-%02d-%02d %02d%02d%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        path = CapturesDir() + name;
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
    Announce(path, png, false);
}

void ScreenshotCatcher::TakeFolder() {
    const std::wstring path = FreshShot(folder_);
    if (path.empty()) return;
    Announce(path, ReadFileBytes(path), true);                            // (Win+PrtSc: only a file, nothing on the clipboard)
}

void ScreenshotCatcher::Announce(const std::wstring& path, const std::vector<uint8_t>& png, bool fromFolder) {
    const ULONGLONG now = GetTickCount64();
    if (path == lastPath_ || (fromFolder && now - lastAt_ < kSameShotMs)) return;   // the same shot, reported twice
    lastPath_ = path;
    lastAt_ = now;
    auto thumb = std::make_shared<ImageBGRA>();
    if (png.empty() || !DecodeImage(png.data(), png.size(), 1.0f, kThumbSide, *thumb)) thumb.reset();
    auto* shot = new Screenshot{path, std::move(thumb)};
    if (!PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(shot))) delete shot;
}

std::wstring ScreenshotCatcher::SaveToScreenshots(const std::wstring& path) {
    const std::wstring folder = KnownFolder(FOLDERID_Screenshots);
    if (folder.empty()) return {};
    std::error_code ec;
    if (fs::equivalent(fs::path(path).parent_path(), folder, ec)) return path;   // it is there already
    const fs::path target = fs::path(folder) / fs::path(path).filename();
    fs::copy_file(path, target, fs::copy_options::overwrite_existing, ec);
    return ec ? std::wstring() : target.wstring();
}

} // namespace eflow
