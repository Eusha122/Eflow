#pragma once
#include <windows.h>

#include <algorithm>
#include <string>

namespace eflow {

// One line of editable text in the island (the Agents prompt, the reminder boxes): the text, the caret, and how far it is
// scrolled so the caret stays in view. Only editing lives here; what Enter, Tab or Esc mean is up to the view that owns it.
struct TextField {
    std::wstring text;
    size_t caret = 0;
    float scroll = 0;
    size_t limit = 8000;

    void Clear() { text.clear(), caret = 0, scroll = 0; }
    void Insert(std::wstring s) {
        s.resize(std::min(s.size(), limit - std::min(limit, text.size())));
        text.insert(caret, s);
        caret += s.size();
    }

    // Typing: printable characters, Backspace and Ctrl+Backspace. True when the character was one of them.
    bool OnChar(wchar_t ch) {
        if (ch == L'\b') {
            if (caret > 0) text.erase(--caret, 1);
        } else if (ch == 0x7F) {                                   // Ctrl+Backspace: the word before the caret
            const size_t end = caret;
            WordLeft();
            text.erase(caret, end - caret);
        } else if (ch >= 0x20) {
            Insert(std::wstring(1, ch));
        } else {
            return false;
        }
        return true;
    }

    // The editing keys: arrows (Ctrl = by word), Home, End, Delete, Ctrl+A, Ctrl+V. True when the key was one of them.
    bool OnKey(WPARAM vk, HWND clipboardOwner) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (vk == VK_LEFT) {
            if (ctrl) WordLeft();
            else if (caret > 0) --caret;
        } else if (vk == VK_RIGHT) {
            if (ctrl) WordRight();
            else if (caret < text.size()) ++caret;
        } else if (vk == VK_HOME) caret = 0;
        else if (vk == VK_END || (ctrl && vk == 'A')) caret = text.size();
        else if (vk == VK_DELETE) { if (caret < text.size()) text.erase(caret, 1); }
        else if (ctrl && vk == 'V') Paste(clipboardOwner);
        else return false;
        return true;
    }

private:
    void WordLeft() {
        while (caret > 0 && text[caret - 1] == L' ') --caret;
        while (caret > 0 && text[caret - 1] != L' ') --caret;
    }
    void WordRight() {
        while (caret < text.size() && text[caret] == L' ') ++caret;
        while (caret < text.size() && text[caret] != L' ') ++caret;
    }
    void Paste(HWND owner) {
        if (!OpenClipboard(owner)) return;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* clip = static_cast<const wchar_t*>(GlobalLock(data))) {
                std::wstring pasted = clip;
                GlobalUnlock(data);
                pasted.erase(std::remove(pasted.begin(), pasted.end(), L'\r'), pasted.end());
                Insert(pasted);
            }
        }
        CloseClipboard();
    }
};

} // namespace eflow
