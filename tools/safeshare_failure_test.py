"""SafeShare's failure paths, end to end, against the real service (no email is sent by any of these).

    python tools/safeshare_failure_test.py

Needs the debug build running. Checks:
  1. a file that was deleted after it was dropped: a clear error, and nothing created on the server;
  2. a folder: refused the same way;
  3. Cancel in the middle of an upload: the half-made room is closed on the server again and nothing is left in the list.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import tempfile
import time

user32 = ctypes.WinDLL("user32")
failures = 0
PHASES = ["Idle", "Preparing", "Uploading", "Inviting", "Sent", "Failed", "Cancelled"]


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
    cds = COPYDATASTRUCT(0x45464C57, len(data), ctypes.cast(buf, ctypes.c_void_p))
    user32.SendMessageW(app_window(), 0x004A, 0, ctypes.byref(cds))


def state():
    path = os.path.join(tempfile.gettempdir(), "eflow-share-state.json")
    send({"cmd": "safeshare_state", "file": path})
    with open(path, encoding="utf-8") as f:
        s = json.load(f)
    s["phaseName"] = PHASES[s["phase"]]
    return s


def wait_for(phases, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        s = state()
        if s["phaseName"] in phases:
            return s
        time.sleep(0.2)
    return state()


def check(ok, what):
    global failures
    print(f"[{'PASS' if ok else 'FAIL'}] {what}")
    if not ok:
        failures += 1


def request(paths):
    return {"cmd": "safeshare_send", "files": paths, "sender": "Eflow failure test", "email": "nobody@example.invalid",
            "password": "test-password-1", "maxDevices": 1}


def main():
    log = os.path.join(os.environ["LOCALAPPDATA"], "Eflow", "eflow.log")
    rooms_before = state()["sent"]

    gone = os.path.join(tempfile.gettempdir(), "eflow-deleted-file.txt")
    with open(gone, "w") as f:
        f.write("x")
    os.remove(gone)
    send(request([gone]))
    s = wait_for({"Failed", "Sent", "Cancelled"}, 20)
    check(s["phaseName"] == "Failed" and "eflow-deleted-file.txt" in s["error"], f"a deleted file: {s['phaseName']}: {s['error']}")

    send(request([tempfile.gettempdir()]))
    s = wait_for({"Failed", "Sent", "Cancelled"}, 20)
    check(s["phaseName"] == "Failed", f"a folder: {s['phaseName']}: {s['error']}")

    big = os.path.join(tempfile.gettempdir(), "eflow-cancel-test.bin")
    with open(big, "wb") as f:
        f.write(os.urandom(120 * 1024 * 1024))                  # 5 parts: long enough to cancel in the middle
    start = os.path.getsize(log)
    try:
        send(request([big]))
        s = wait_for({"Uploading", "Failed"}, 60)
        check(s["phaseName"] == "Uploading", f"uploading ({s['phaseName']} {s['error']})")
        time.sleep(3)
        send({"cmd": "safeshare_cancel"})
        s = wait_for({"Cancelled", "Failed", "Sent"}, 60)
        check(s["phaseName"] == "Cancelled" and s["error"] == "", f"cancelled: {s['phaseName']} {s['error']}")
        time.sleep(3)                                           # (the room is closed right after)
        with open(log, "rb") as f:
            f.seek(start)
            new = f.read().decode("utf-8", "replace")
        check("closing a room failed" not in new and "invite" not in new, "the half-made room was closed; no email")
        check(state()["sent"] == rooms_before, "nothing new in the list of sent rooms")
        store = json.load(open(os.path.join(os.environ["LOCALAPPDATA"], "Eflow", "safeshare.json"), encoding="utf-8"))
        check(all(r["complete"] for r in store["rooms"]), "no half-made room is kept on disk")
    finally:
        send({"cmd": "safeshare_cancel"})
        os.remove(big)
        send({"cmd": "safeshare_dismiss"})
    print("\nall passed" if not failures else f"\n{failures} failed")
    raise SystemExit(1 if failures else 0)


if __name__ == "__main__":
    main()
