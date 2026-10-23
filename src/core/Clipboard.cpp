#include "core/Clipboard.h"

#include <cstring>

namespace eflow {
namespace {

bool SetData(UINT format, const void* data, size_t size) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return false;
    if (void* p = GlobalLock(mem)) {
        memcpy(p, data, size);
        GlobalUnlock(mem);
        if (SetClipboardData(format, mem)) return true;          // the clipboard owns it now
    }
    GlobalFree(mem);
    return false;
}

} // namespace

bool CopyText(HWND owner, const std::wstring& text, bool secret) {
    bool opened = false;
    for (int attempt = 0; attempt < 5 && !(opened = OpenClipboard(owner) != FALSE); ++attempt) Sleep(20);
    if (!opened) return false;
    EmptyClipboard();
    const bool ok = SetData(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    if (ok && secret) {
        const DWORD no = 0;
        SetData(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), &no, sizeof(no));
        SetData(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), &no, sizeof(no));
        SetData(RegisterClipboardFormatW(L"CanUploadToCloudClipboard"), &no, sizeof(no));
    }
    CloseClipboard();
    return ok;
}

} // namespace eflow
