#pragma once
#include <windows.h>

#include <string>

namespace eflow {

// Puts `text` on the clipboard. `secret`: Windows' clipboard history (Win+V) and the cloud clipboard leave it out (a login
// code, a password, a link that carries a key). Returns false when another app holds the clipboard.
bool CopyText(HWND owner, const std::wstring& text, bool secret);

} // namespace eflow
