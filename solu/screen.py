"""Solu's eyes: capture the window the user is working in (only when asked), read its text with Windows' own OCR.

Nothing here runs in the background and nothing is stored: a capture exists for the length of one question.
"""
import asyncio
import base64
import ctypes
import difflib
import io
import re
from ctypes import wintypes

from PIL import Image, ImageGrab

try:                                                      # the per-monitor DPI makes window sizes match real pixels
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:                                         # noqa: BLE001
    pass

user32, gdi32, kernel32 = ctypes.windll.user32, ctypes.windll.gdi32, ctypes.windll.kernel32
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
user32.GetWindowDC.argtypes = [wintypes.HWND]
user32.GetWindowDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
kernel32.OpenProcess.restype = wintypes.HANDLE

SHELL_CLASSES = {"Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd"}
# Windows that are never captured: the question is answered without looking.
SENSITIVE = ("password", "1password", "bitwarden", "keepass", "lastpass", "dashlane", "authenticator", "private key", "seed phrase", "recovery phrase")


class Look:
    """What Solu saw: the window's title and program, the picture, and (once read) its text."""

    def __init__(self, title, app, image, scope):
        self.title, self.app, self.image, self.scope = title, app, image, scope
        self.text = ""
        self.blocked = None                              # a reason, when the window must not be looked at
        self.origin = (0, 0)                             # where the picture's top-left corner is on the screen


def _text(fn, hwnd):
    buf = ctypes.create_unicode_buffer(512)
    fn(hwnd, buf, 512)
    return buf.value


def _process_name(hwnd):
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    handle = kernel32.OpenProcess(0x1000, False, pid.value)          # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(1024)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            name = buf.value.rsplit("\\", 1)[-1]
            return name[:-4] if name.lower().endswith(".exe") else name
    finally:
        kernel32.CloseHandle(handle)
    return ""


def _print_window(hwnd):
    """The window's own pixels (nothing in front of it can leak in); None if the app does not support it."""
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w < 40 or h < 40:
        return None
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    old = gdi32.SelectObject(mem, bmp)
    ok = user32.PrintWindow(hwnd, mem, 2)                            # PW_RENDERFULLCONTENT: includes GPU-drawn browsers and Electron
    image = None
    if ok:
        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("w", wintypes.LONG), ("h", wintypes.LONG), ("planes", wintypes.WORD), ("bits", wintypes.WORD),
                        ("compression", wintypes.DWORD), ("sizeimage", wintypes.DWORD), ("xppm", wintypes.LONG), ("yppm", wintypes.LONG),
                        ("used", wintypes.DWORD), ("important", wintypes.DWORD)]
        bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        buffer = ctypes.create_string_buffer(w * h * 4)
        gdi32.GetDIBits(mem, bmp, 0, h, buffer, ctypes.byref(bi), 0)
        image = Image.frombuffer("RGBA", (w, h), buffer, "raw", "BGRA", 0, 1).convert("RGB")
    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    if image is not None and image.getextrema() == ((0, 0), (0, 0), (0, 0)):   # an all-black result: the app drew nothing for us
        return None
    return image


def look_at_foreground(whole_screen=False):
    """Capture the window the user is in (or the whole desktop). Returns a Look; `.blocked` says why not, if it must not be seen."""
    hwnd = user32.GetForegroundWindow()
    title = _text(user32.GetWindowTextW, hwnd) if hwnd else ""
    cls = _text(user32.GetClassNameW, hwnd) if hwnd else ""
    app = _process_name(hwnd) if hwnd else ""
    if any(word in (title + " " + app).lower() for word in SENSITIVE):
        look = Look(title, app, None, "window")
        look.blocked = "That looks like a password or security window, so I won't look at it."
        return look
    if whole_screen or not hwnd or cls in SHELL_CLASSES:
        look = Look("Your screen", "Windows", ImageGrab.grab(all_screens=True).convert("RGB"), "screen")
        look.origin = (user32.GetSystemMetrics(76), user32.GetSystemMetrics(77))   # the virtual screen's corner
        return look
    image = _print_window(hwnd)
    if image is None:                                                 # fall back to the pixels on screen
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        image = ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom), all_screens=True).convert("RGB")
    look = Look(title, app, image, "window")
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    look.origin = (rect.left, rect.top)
    return look


async def _ocr(png):
    from winrt.windows.graphics.imaging import BitmapDecoder
    from winrt.windows.media.ocr import OcrEngine
    from winrt.windows.storage.streams import DataWriter, InMemoryRandomAccessStream
    stream = InMemoryRandomAccessStream()
    writer = DataWriter(stream)
    writer.write_bytes(png)
    await writer.store_async()
    writer.detach_stream()
    stream.seek(0)
    decoder = await BitmapDecoder.create_async(stream)
    bitmap = await decoder.get_software_bitmap_async()
    engine = OcrEngine.try_create_from_user_profile_languages()
    if engine is None:
        return ""
    result = await engine.recognize_async(bitmap)
    return "\n".join(line.text for line in result.lines)


async def _ocr_words(png):
    from winrt.windows.graphics.imaging import BitmapDecoder
    from winrt.windows.media.ocr import OcrEngine
    from winrt.windows.storage.streams import DataWriter, InMemoryRandomAccessStream
    stream = InMemoryRandomAccessStream()
    writer = DataWriter(stream)
    writer.write_bytes(png)
    await writer.store_async()
    writer.detach_stream()
    stream.seek(0)
    bitmap = await (await BitmapDecoder.create_async(stream)).get_software_bitmap_async()
    engine = OcrEngine.try_create_from_user_profile_languages()
    if engine is None:
        return []
    result = await engine.recognize_async(bitmap)
    return [[(w.text, w.bounding_rect.x, w.bounding_rect.y, w.bounding_rect.width, w.bounding_rect.height) for w in line.words]
            for line in result.lines]


def _norm(word):
    return re.sub(r"[^\w]", "", word.lower())


def words_of(look):
    """The words Windows reads on the picture, with their boxes: one OCR pass per capture (reading and finding share it)."""
    if getattr(look, "words", None) is None:
        image, look.scale = look.image, 1.0
        if image.width > 3840:                                         # the OCR engine has a size limit
            look.scale = 3840 / image.width
            image = image.resize((3840, int(image.height * look.scale)))
        buf = io.BytesIO()
        image.save(buf, "PNG")
        try:
            look.words = asyncio.run(_ocr_words(buf.getvalue()))
        except Exception:                                              # noqa: BLE001
            look.words = []
    return look.words


def find_text(look, wanted):
    """Where `wanted` is written on the captured window: (screen x, screen y, what was read there), or None."""
    lines, scale = words_of(look), look.scale
    target = [_norm(w) for w in wanted.split() if _norm(w)]
    if not target:
        return None
    best, best_score = None, 0.0
    for words in lines:
        normed = [_norm(w[0]) for w in words]
        for i in range(len(words)):
            for j in range(i + 1, min(len(words), i + len(target) + 2) + 1):
                score = difflib.SequenceMatcher(None, " ".join(normed[i:j]), " ".join(target)).ratio()
                if score > best_score:
                    best_score, best = score, words[i:j]
    if best is None or best_score < 0.75:
        return None
    left = min(w[1] for w in best)
    right = max(w[1] + w[3] for w in best)
    top = min(w[2] for w in best)
    bottom = max(w[2] + w[4] for w in best)
    x = look.origin[0] + (left + right) / 2 / scale
    y = look.origin[1] + (top + bottom) / 2 / scale
    return x, y, " ".join(w[0] for w in best)


def read_text(look):
    """The text on the captured picture, as Windows reads it (a few hundred milliseconds, once per capture)."""
    look.text = "\n".join(" ".join(w[0] for w in line) for line in words_of(look)).strip()
    return look.text


def thumbnail_b64(look, width=176):
    """A small JPEG of what was seen: Eflow shows it so it is always clear what Solu looked at."""
    image = look.image.copy()
    image.thumbnail((width, width))
    buf = io.BytesIO()
    image.save(buf, "JPEG", quality=70)
    return base64.b64encode(buf.getvalue()).decode()


def picture_b64(look, width=896):
    """The picture for the vision model."""
    image = look.image
    if image.width > width:
        image = image.resize((width, int(image.height * width / image.width)))
    buf = io.BytesIO()
    image.save(buf, "JPEG", quality=85)
    return base64.b64encode(buf.getvalue()).decode()
