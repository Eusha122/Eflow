#include "services/ShelfStore.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Utf8.h"
#include "core/Json.h"
#include "core/Log.h"

namespace eflow {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxItems = 60;

std::wstring StorePath() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    dir += L"\\Eflow";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\shelf.json";
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

void Describe(ShelfItem& item) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &data)) {
        item.missing = false;
        item.isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    } else {
        item.missing = true;
    }
    const size_t cut = item.path.find_last_of(L"\\/");
    item.name = cut == std::wstring::npos ? item.path : item.path.substr(cut + 1);
    if (item.name.empty()) item.name = item.path;                       // a drive root
}

// A thumbnail (pictures, videos, documents) or the icon of the file, as premultiplied BGRA of about 96 px.
std::shared_ptr<const ImageBGRA> Picture(const std::wstring& path) {
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
    HBITMAP bmp = nullptr;
    HRESULT hr = factory->GetImage(SIZE{96, 96}, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &bmp);   // a thumbnail where there is one
    factory->Release();
    if (FAILED(hr) || !bmp) return nullptr;
    BITMAP info{};
    GetObjectW(bmp, sizeof(info), &info);
    auto image = std::make_shared<ImageBGRA>();
    if (info.bmWidth > 0 && info.bmHeight > 0) {
        image->width = static_cast<uint32_t>(info.bmWidth);
        image->height = static_cast<uint32_t>(info.bmHeight);
        image->pixels.assign(static_cast<size_t>(image->width) * image->height * 4, 0);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = info.bmWidth;
        bi.bmiHeader.biHeight = -info.bmHeight;                          // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, bmp, 0, image->height, image->pixels.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        bool anyAlpha = false;
        for (size_t i = 3; i < image->pixels.size(); i += 4)
            if (image->pixels[i]) { anyAlpha = true; break; }
        if (!anyAlpha)                                                   // a thumbnail without an alpha channel: opaque
            for (size_t i = 3; i < image->pixels.size(); i += 4) image->pixels[i] = 255;
    }
    DeleteObject(bmp);
    return image->pixels.empty() ? nullptr : image;
}

} // namespace

ShelfStore::ShelfStore(HWND notify, UINT message) : notify_(notify), message_(message) {}

ShelfStore::~ShelfStore() {
    closing_ = true;
    for (std::thread& t : threads_)
        if (t.joinable()) t.join();
}

void ShelfStore::Changed() {
    ++version_;
    if (!posted_.exchange(true)) PostMessageW(notify_, message_, 0, 0);
}

void ShelfStore::Load() {
    std::ifstream file(StorePath(), std::ios::binary);
    if (file) {
        std::stringstream text;
        text << file.rdbuf();
        json::Value root;
        std::string error;
        if (json::Parse(text.str(), root, error) && root.type == json::Value::Type::Object)
            if (json::Value* list = root.Find("items"); list && list->type == json::Value::Type::Array)
                for (json::Value& v : list->items)
                    if (v.type == json::Value::Type::String && !v.text.empty() && items_.size() < kMaxItems) {
                        ShelfItem item;
                        item.path = FromUtf8(v.text);
                        Describe(item);
                        if (!item.missing) items_.push_back(std::move(item));       // a file that is gone is not brought back
                    }
    }
    ++version_;
    LoadIcons();
}

void ShelfStore::Save() {
    json::Value root = json::Value::Object();
    json::Value list = json::Value::Array();
    for (const ShelfItem& i : items_) list.items.push_back(json::Value::String(ToUtf8(i.path)));
    root.Member("items") = std::move(list);
    std::ofstream file(StorePath(), std::ios::binary | std::ios::trunc);
    if (file) file << json::Write(root);
}

int ShelfStore::Add(const std::vector<std::wstring>& paths) {
    int added = 0;
    {
        std::lock_guard lock(mutex_);
        for (const std::wstring& p : paths) {
            if (p.empty() || items_.size() >= kMaxItems) continue;
            const bool known = std::any_of(items_.begin(), items_.end(), [&](const ShelfItem& i) { return Lower(i.path) == Lower(p); });
            if (known) continue;
            ShelfItem item;
            item.path = p;
            Describe(item);
            if (item.missing) continue;
            items_.push_back(std::move(item));
            ++added;
        }
        if (added) Save();
    }
    if (added) {
        Changed();
        LoadIcons();
    }
    return added;
}

void ShelfStore::Remove(const std::wstring& path) {
    {
        std::lock_guard lock(mutex_);
        std::erase_if(items_, [&](const ShelfItem& i) { return Lower(i.path) == Lower(path); });
        Save();
    }
    Changed();
}

void ShelfStore::Clear() {
    {
        std::lock_guard lock(mutex_);
        items_.clear();
        Save();
    }
    Changed();
}

ShelfSnapshot ShelfStore::Snapshot() {
    std::lock_guard lock(mutex_);
    posted_ = false;
    ShelfSnapshot snap;
    snap.items = items_;
    snap.version = version_;
    return snap;
}

void ShelfStore::LoadIcons() {
    std::vector<std::wstring> todo;
    {
        std::lock_guard lock(mutex_);
        for (const ShelfItem& i : items_)
            if (!i.icon) todo.push_back(i.path);
    }
    if (todo.empty() || iconThreads_ > 0) return;                       // one worker at a time; it picks up everything that is missing
    ++iconThreads_;
    threads_.emplace_back([this] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            std::wstring path;
            {
                std::lock_guard lock(mutex_);
                for (const ShelfItem& i : items_)
                    if (!i.icon && !i.missing) { path = i.path; break; }
            }
            if (path.empty() || closing_) {
                --iconThreads_;
                bool more = false;                                       // something may have been added while this one was finishing
                {
                    std::lock_guard lock(mutex_);
                    for (const ShelfItem& i : items_) more |= !i.icon && !i.missing;
                }
                if (more && !closing_) { ++iconThreads_; continue; }
                CoUninitialize();
                return;
            }
            auto picture = Picture(path);
            {
                std::lock_guard lock(mutex_);
                for (ShelfItem& i : items_)
                    if (i.path == path) i.icon = picture ? picture : std::make_shared<ImageBGRA>(ImageBGRA{1, 1, {0, 0, 0, 0}});   // never retried
            }
            Changed();
        }
    });
}

} // namespace eflow
