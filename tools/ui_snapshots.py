"""Layout snapshots of every island tab, with scripted (fake) content, for before/after comparisons.

    python tools/ui_snapshots.py <out-dir>

Needs the debug build running. It turns on shoot mode (the real notifications, agents, Solu, shelf and system feeds are
set aside, so nothing private is drawn), fills each tab with demo content, opens the tabs one by one and asks the island
to save its own frame as a PNG (the island's pixels only, never the screen). Shoot mode is switched off at the end, even
when something fails. Music and Reminders show their real content.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
WM_COPYDATA = 0x004A
MAGIC = 0x45464C57                       # 'EFLW': the shoot director's commands
SETTLE_S = 1.4                           # a tab switch and the height spring settle well within this
TABS = ["music", "notifications", "solu", "agents", "system", "shelf", "reminders", "safeshare"]
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


class COPYDATASTRUCT(ctypes.Structure):
    _fields_ = [("dwData", ctypes.c_size_t), ("cbData", wt.DWORD), ("lpData", ctypes.c_void_p)]


def app_window():
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def visit(hwnd, _):
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, name, 64)
        if name.value == "Eflow.App":
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(visit, 0)
    if not found:
        raise SystemExit("Eflow (debug build) is not running")
    return found[0]


def send(cmd):
    data = json.dumps(cmd, ensure_ascii=False).encode("utf-8")
    buf = ctypes.create_string_buffer(data, len(data))
    cds = COPYDATASTRUCT(MAGIC, len(data), ctypes.cast(buf, ctypes.c_void_p))
    user32.SendMessageW(app_window(), WM_COPYDATA, 0, ctypes.byref(cds))


def demo_content():
    for title, body in [("Rahim", "Are we still on for 7?"), ("Design team", "New mockups are in the shared folder"),
                        ("Mum", "Call me when you are free")]:
        send({"cmd": "notify", "title": title, "body": body, "quiet": True})
    send({"cmd": "agents", "sessions": [{"tool": "claude", "id": "demo", "project": "eflow", "title": "Polish the island",
                                         "status": "working", "doing": "Editing IslandWindow.cpp",
                                         "feed": [{"kind": "tool", "text": "Read IslandWindow.cpp", "ago": 40},
                                                  {"kind": "text", "text": "The tab row now has room for eight tabs.", "ago": 12}]}]})
    send({"cmd": "system", "cpu": 23, "gpu": 9, "memory": 41, "network": 2})
    send({"cmd": "shelf", "files": [os.path.join(REPO, "README.md"), os.path.join(REPO, "docs", "ARCHITECTURE.md")]})


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    send({"cmd": "shoot", "on": True})
    try:
        demo_content()
        for index, name in enumerate(TABS):
            send({"cmd": "tab", "section": index})
            time.sleep(SETTLE_S)
            path = os.path.join(out, f"{index}-{name}.png")
            send({"cmd": "snapshot", "file": path})
            time.sleep(0.3)
            print(path if os.path.exists(path) else f"{path}: not written (no such tab?)")
    finally:
        send({"cmd": "tab", "section": -1})
        send({"cmd": "shoot", "on": False})


if __name__ == "__main__":
    main()
