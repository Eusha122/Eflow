#include "island/CaptureOverlay.h"

#include <windowsx.h>

#include <algorithm>

namespace eflow {
namespace {

const wchar_t kClass[] = L"Eflow.Capture";
constexpr COLORREF kHole = RGB(1, 2, 3);          // painted where the selection is: fully see-through
constexpr BYTE kDim = 120;                        // how dark the rest of the screen gets

} // namespace

CaptureOverlay::~CaptureOverlay() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void CaptureOverlay::Start(const std::wstring& mode, std::function<void(RECT, const std::wstring&)> done) {
    if (hwnd_) return;
    mode_ = mode;
    done_ = std::move(done);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProcThunk;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    origin_ = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"Show Solu", WS_POPUP, origin_.x, origin_.y,
                            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) return;
    SetLayeredWindowAttributes(hwnd_, kHole, kDim, LWA_ALPHA | LWA_COLORKEY);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);                   // so Esc reaches us
    dragging_ = false;
}

LRESULT CALLBACK CaptureOverlay::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->WndProc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

RECT CaptureOverlay::Selection() const {
    return {std::min(from_.x, to_.x), std::min(from_.y, to_.y), std::max(from_.x, to_.x), std::max(from_.y, to_.y)};
}

void CaptureOverlay::Finish(bool ok) {
    const RECT sel = Selection();
    HWND hwnd = std::exchange(hwnd_, nullptr);
    if (GetCapture() == hwnd) ReleaseCapture();
    DestroyWindow(hwnd);
    if (ok && sel.right - sel.left >= 12 && sel.bottom - sel.top >= 12 && done_) {
        RECT screen{sel.left + origin_.x, sel.top + origin_.y, sel.right + origin_.x, sel.bottom + origin_.y};
        done_(screen, mode_);
    }
}

LRESULT CaptureOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        from_ = to_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        dragging_ = true;
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_) {
            to_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            to_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            Finish(true);
        }
        return 0;
    case WM_RBUTTONDOWN:
        Finish(false);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) Finish(false);
        return 0;
    case WM_KILLFOCUS:
        if (!dragging_ && hwnd_) Finish(false);   // the user went elsewhere: give up quietly
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT all;
        GetClientRect(hwnd, &all);
        HDC mem = CreateCompatibleDC(dc);                    // double-buffered: no flicker while dragging
        HBITMAP bmp = CreateCompatibleBitmap(dc, all.right, all.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(mem, &all, black);
        DeleteObject(black);
        if (dragging_) {
            const RECT sel = Selection();
            HBRUSH hole = CreateSolidBrush(kHole);
            FillRect(mem, &sel, hole);
            DeleteObject(hole);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ oldPen = SelectObject(mem, pen);
            HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, sel.left - 1, sel.top - 1, sel.right + 1, sel.bottom + 1);
            SelectObject(mem, oldBrush);
            SelectObject(mem, oldPen);
            DeleteObject(pen);
        } else {                                             // a hint until the drag starts
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(255, 255, 255));
            HFONT font = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ oldFont = SelectObject(mem, font);
            RECT hint = all;
            hint.bottom = all.bottom / 2;
            const wchar_t* text = mode_ == L"translate" ? L"Drag over what Solu should translate  ·  Esc to cancel"
                                                        : L"Drag over what you want to show Solu  ·  Esc to cancel";
            DrawTextW(mem, text, -1, &hint, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
            SelectObject(mem, oldFont);
            DeleteObject(font);
        }
        BitBlt(dc, 0, 0, all.right, all.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace eflow
