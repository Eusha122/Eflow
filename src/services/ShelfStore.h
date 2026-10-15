#pragma once
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Image.h"

namespace eflow {

struct ShelfItem {
    std::wstring path, name;
    bool isDir = false;
    bool missing = false;                        // the file was moved or deleted since it was put on the shelf
    uint64_t size = 0;
    std::shared_ptr<const ImageBGRA> icon;       // a thumbnail for pictures and videos, the file's icon otherwise; null until loaded
};

struct ShelfSnapshot {
    std::vector<ShelfItem> items;                // oldest first
    uint32_t version = 0;
};

// The file shelf: files the user dropped on the island, held by reference (nothing is copied or moved). Saved between runs,
// each item's thumbnail / icon is fetched on a worker thread. Posts `message` to `notify` when the list or an icon changed.
class ShelfStore {
public:
    ShelfStore(HWND notify, UINT message);
    ~ShelfStore();
    ShelfStore(const ShelfStore&) = delete;
    ShelfStore& operator=(const ShelfStore&) = delete;

    void Load();
    int Add(const std::vector<std::wstring>& paths);   // returns how many were new
    void Remove(const std::wstring& path);
    void Clear();
    ShelfSnapshot Snapshot();

private:
    void Save();
    void LoadIcons();
    void Changed();

    HWND notify_;
    UINT message_;
    std::mutex mutex_;
    std::vector<ShelfItem> items_;
    uint32_t version_ = 0;
    std::atomic<bool> posted_{false};
    std::atomic<int> iconThreads_{0};
    std::atomic<bool> closing_{false};
    std::vector<std::thread> threads_;
};

} // namespace eflow
