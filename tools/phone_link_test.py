"""End-to-end test of the phone link (Phone <-> PC on the same Wi-Fi), with this PC playing the phone.

    python tools/phone_link_test.py

Needs the debug build running. It starts the link through Eflow's debug commands (no clicks), then checks the approval
flow (including a phone that opens the link twice: the camera's preview, then the browser), a download, an upload, and
that a wrong link gets nothing. Devices at other addresses are covered by the unit tests (tests/share_test.cpp), since
every request here comes from this PC. What it sends lands in Downloads\\From phone and on the Shelf; both are cleaned up
at the end, and the link is stopped, even when a check fails.
"""
import ctypes
import ctypes.wintypes as wt
import http.cookiejar
import json
import os
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

user32 = ctypes.WinDLL("user32")
failures = 0


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
    path = os.path.join(tempfile.gettempdir(), "eflow-phone-state.json")
    send({"cmd": "phone", "do": "state", "file": path})
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def check(ok, what):
    global failures
    print(f"[{'PASS' if ok else 'FAIL'}] {what}")
    if not ok:
        failures += 1


def browser():
    return urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))


def get(opener, url, data=None, method=None):
    try:
        with opener.open(urllib.request.Request(url, data=data, method=method), timeout=15) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


def main():
    downloads = os.path.join(os.path.expanduser("~"), "Downloads", "From phone")
    before = set(os.listdir(downloads)) if os.path.isdir(downloads) else set()
    offered = os.path.join(tempfile.gettempdir(), "eflow phone offer ✓.bin")
    payload = os.urandom(3 * 1024 * 1024 + 7)
    with open(offered, "wb") as f:
        f.write(payload)
    saved = []
    try:
        send({"cmd": "phone", "do": "start"})
        for _ in range(50):
            s = state()
            if s["state"] in (2, 5):                              # Waiting, Unavailable
                break
            time.sleep(0.1)
        if s["state"] == 5 and "public" in s["reason"]:         # the user's own Wi-Fi may be called public: "It's my Wi-Fi"
            print(f"[INFO] {s['reason']}")
            send({"cmd": "phone", "do": "start_public"})
            for _ in range(50):
                s = state()
                if s["state"] in (2, 5):
                    break
                time.sleep(0.1)
        if s["state"] == 5:
            raise SystemExit(f"the link could not start: {s['reason']}")
        url = s["url"]
        check(s["state"] == 2 and url.startswith("http://") and "/s/" in url and s["qrSize"] >= 21, f"started, waiting, QR {s['qrSize']}x{s['qrSize']}")
        base = url.rstrip("/")
        token = base.rsplit("/", 1)[1]
        check(len(token) == 22, "the link carries a 128-bit token")

        stranger = browser()
        code, _, _ = get(stranger, base.replace(token, "A" * 22) + "/")
        check(code == 404, "a wrong token gets nothing (404)")

        preview = browser()                                   # the camera app's preview loads the link first
        code, _, _ = get(preview, url)
        check(code == 200, "the camera's preview loads the page")
        phone = browser()                                     # then the browser opens it, without the preview's cookie
        code, page, headers = get(phone, url)
        check(code == 200 and b"Waiting for your PC" in page and "default-src 'none'" in headers.get("Content-Security-Policy", ""),
              "the page loads, with a strict Content-Security-Policy")
        s = state()
        check(s["state"] == 3 and "at" in s["device"], f"the PC is asked: {s['device']}")
        code, body, _ = get(phone, url + "api/state")
        check(json.loads(body)["status"] == "waiting", "the phone waits for the answer")
        code, _, _ = get(phone, url + "api/upload?name=x.txt", data=b"x", method="POST")
        check(code == 403, "before Allow, an upload is refused")

        code, body, _ = get(preview, url + "api/state")
        check(json.loads(body)["status"] == "waiting", "the preview and the browser are the same phone (not 'another phone')")

        send({"cmd": "phone", "do": "allow"})
        check(state()["state"] == 4, "allowed: connected")
        send({"cmd": "phone", "do": "offer", "files": [offered]})
        code, body, _ = get(phone, url + "api/state")
        files = json.loads(body)["files"]
        check(code == 200 and len(files) == 1 and files[0]["name"] == os.path.basename(offered) and files[0]["size"] == len(payload),
              "the phone sees the offered file (non-ASCII name, exact size)")
        code, body, headers = get(phone, url + "api/file/" + urllib.parse.quote(files[0]["id"]))
        check(code == 200 and body == payload and "filename*=UTF-8''" in headers.get("Content-Disposition", ""), "download: byte for byte")
        cookieless = browser()
        code, body, _ = get(cookieless, url + "api/state")
        check(json.loads(body)["status"] == "allowed", "the allowed phone gets in even without its cookie")

        sent = os.urandom(1024 * 1024 + 3)
        code, body, _ = get(phone, url + "api/upload?name=" + urllib.parse.quote("../..\\evil:name?.txt"), data=sent, method="POST")
        check(code == 200, "upload accepted")
        time.sleep(0.5)
        after = set(os.listdir(downloads)) - before
        saved = [os.path.join(downloads, n) for n in after]
        check(len(after) == 1 and next(iter(after)) == "evil_name_.txt", f"saved under a safe name: {sorted(after)}")
        check(saved and open(saved[0], "rb").read() == sent, "upload: byte for byte")
        check(state()["received"] == 1, "the PC counts it")
        code, _, _ = get(phone, url + "api/upload?name=CON.txt", data=b"reserved", method="POST")
        time.sleep(0.3)
        extra = set(os.listdir(downloads)) - before - after
        saved += [os.path.join(downloads, n) for n in extra]
        check(code == 200 and extra == {"_CON.txt"}, f"a reserved device name is made safe: {sorted(extra)}")
        req = urllib.request.Request(url + "api/upload?name=big.bin", data=b"", method="POST", headers={"Content-Length": str(5 * 1024 ** 3)})
        try:
            with phone.open(req, timeout=15) as r:
                code = r.status
        except urllib.error.HTTPError as e:
            code = e.code
        except Exception:
            code = -1
        check(code == 413, "an upload over 4 GB is refused before it starts")
    finally:
        send({"cmd": "phone", "do": "stop"})
        time.sleep(0.5)
        for p in saved:
            send({"cmd": "shelf_remove", "file": p})
            try:
                os.remove(p)
            except OSError:
                pass
        os.remove(offered)
    check(state()["state"] == 0, "stopped")
    try:
        urllib.request.urlopen(url, timeout=3)
        check(False, "after Stop nothing answers")
    except Exception:
        check(True, "after Stop nothing answers")
    print("\nall passed" if not failures else f"\n{failures} failed")
    raise SystemExit(1 if failures else 0)


if __name__ == "__main__":
    main()
