"""Solu's hands: the things it can do on the PC, described for the brain (Ollama tool calling) and carried out here.

Every tool has a permission level:
  auto     harmless and expected (music keys, volume, a timer, the time): done at once
  ask      the island shows an Approve / Reject card first (the user can also say "yes" / "no")
Some "ask" tools can be trusted for good ("Always allow"); commands, file writes and closing windows never can.
"""
import ctypes
import difflib
import json
import os
import re
import subprocess
import threading
import time
import urllib.parse
from ctypes import wintypes

user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
NO_WINDOW = 0x08000000
TRUST_FILE = os.path.join(os.environ.get("LOCALAPPDATA", "."), "Eflow", "solu_trust.json")
HOME = os.path.expanduser("~")


def _tool(name, description, properties=None, required=()):
    return {"type": "function", "function": {"name": name, "description": description,
            "parameters": {"type": "object", "properties": properties or {}, "required": list(required)}}}


def _s(description):
    return {"type": "string", "description": description}


def _n(description):
    return {"type": "number", "description": description}


TOOLS = [
    _tool("open_app", "Open (launch) an installed program or Windows app by name, e.g. Chrome, Spotify, Calculator, Settings, File Explorer.",
          {"name": _s("the app's name")}, ["name"]),
    _tool("open_website", "Open a web address in the browser.", {"url": _s("the address, e.g. youtube.com")}, ["url"]),
    _tool("web_search", "Search the web in the browser (opens the results page for the user).", {"query": _s("what to search for")}, ["query"]),
    _tool("research", "Look something up on the internet and READ it: returns the top search results and the text of the best pages, "
          "so you can answer, summarize or write about it. Use this (not web_search) when you need the information yourself.",
          {"query": _s("what to look up")}, ["query"]),
    _tool("deep_research", "Start a research project that runs in the background: Solu searches many sources, reads them and writes a full "
          "report (summary, findings, sections, sources) that opens in Chrome when it is ready. Use it when the user asks for deep or "
          "in-depth research, a report or a document about a topic. depth 'deep' (thorough, a few minutes) or 'standard' (quicker).",
          {"topic": _s("what to research, in the user's words"), "depth": {"type": "string", "enum": ["deep", "standard"]}}, ["topic"]),
    _tool("send_email", "Write an email in the user's Gmail. The user sees the whole email on an approval card first. With a clear address "
          "it is sent; if the address is missing or unclear, leave 'to' empty and it opens as a draft for the user to add the address. "
          "Write the complete subject and body yourself.",
          {"to": _s("the recipient's email address, or empty if you don't have a clear one"), "subject": _s("subject line"),
           "body": _s("the full email text")}, ["subject", "body"]),
    _tool("media_control", "Control the music or video that is playing.",
          {"action": {"type": "string", "enum": ["play_pause", "next", "previous", "stop"]}}, ["action"]),
    _tool("set_volume", "Change the PC's sound volume.",
          {"percent": _n("set the volume to this level, 0-100"), "change": _n("or change it by this many percent, e.g. 10 or -10"),
           "mute": {"type": "boolean", "description": "true to mute, false to unmute"}}),
    _tool("set_timer", "Start a timer; Solu says when it is done.",
          {"seconds": _n("length of the timer in seconds"), "label": _s("what it is for, e.g. tea")}, ["seconds"]),
    _tool("system_info", "The current time and date, battery, how long the PC has been on, memory use and free disk space on every drive."),
    _tool("look_at_screen", "Read the text on the window the user is looking at (its title, app and visible text)."),
    _tool("click_text", "Click on a button, link or any text visible in the window the user is looking at.",
          {"text": _s("the words shown on the thing to click, exactly as they appear")}, ["text"]),
    _tool("type_text", "Type text into the window the user is working in, as if typed on the keyboard.",
          {"text": _s("the text to type")}, ["text"]),
    _tool("press_keys", "Press a key or a keyboard shortcut, e.g. 'enter', 'ctrl+s', 'alt+tab', 'win+d', 'ctrl+shift+esc'.",
          {"keys": _s("the keys, joined with +")}, ["keys"]),
    _tool("window_control", "List the open windows, or bring one to the front, minimize, maximize, restore, snap it to the left or right "
          "half of the screen, or close it. Leave the title empty for the window the user is in.",
          {"action": {"type": "string", "enum": ["list", "focus", "minimize", "maximize", "restore", "snap_left", "snap_right", "close"]},
           "title": _s("part of the window's title or app name; empty = the current window")}, ["action"]),
    _tool("scroll", "Scroll the window the user is in.",
          {"direction": {"type": "string", "enum": ["down", "up", "top", "bottom"]}, "pages": _n("how many screens to scroll, default 1")},
          ["direction"]),
    _tool("find_files", "Find files or folders anywhere on the PC (every drive). Give the name the way the user said it: the search understands "
          "loose names, abbreviations (Game of Thrones = GOT) and seasons (season 1 = S01), and returns the closest matches first.",
          {"name": _s("what to look for, e.g. 'game of thrones season 1'"),
           "folder": _s("only if the user named a place: a drive like 'I' or a folder like 'Downloads'")}, ["name"]),
    _tool("open_file", "Open a file or folder with its usual program.", {"path": _s("full path")}, ["path"]),
    _tool("read_file", "Read a text file (first part).", {"path": _s("full path")}, ["path"]),
    _tool("write_file", "Create or overwrite a text file (or add to its end).",
          {"path": _s("full path"), "content": _s("the text"), "append": {"type": "boolean"}}, ["path", "content"]),
    _tool("clipboard", "Read the clipboard, or copy text to it.",
          {"action": {"type": "string", "enum": ["read", "copy"]}, "text": _s("text to copy")}, ["action"]),
    _tool("run_command", "Run a PowerShell command on the PC and get its output. Use only when no other tool can do it.",
          {"command": _s("the PowerShell command")}, ["command"]),
]

# name -> (level, can be always-allowed)
LEVEL = {"research": ("auto", False), "deep_research": ("auto", False), "send_email": ("ask", False), "open_app": ("auto", False), "open_website": ("auto", False),
         "scroll": ("auto", False), "web_search": ("auto", False), "media_control": ("auto", False),
         "set_volume": ("auto", False), "set_timer": ("auto", False), "system_info": ("auto", False), "look_at_screen": ("auto", False),
         "click_text": ("ask", True), "type_text": ("ask", True), "press_keys": ("ask", True), "window_control": ("auto", False),
         "find_files": ("ask", True), "open_file": ("auto", True), "read_file": ("ask", True), "write_file": ("ask", False),
         "clipboard": ("ask", True), "run_command": ("ask", False)}

RISKY = re.compile(r"\b(remove-item|rm|del|erase|rmdir|rd|format(-volume)?|clear-disk|shutdown|restart-computer|stop-computer|"
                   r"reg\s+delete|remove-itemproperty|set-executionpolicy|diskpart|bcdedit|cipher|takeown|icacls|stop-process|taskkill|"
                   r"iex|invoke-expression|set-mppreference|disable-|uninstall)\b", re.I)


# ---- which tools a request may use -----------------------------------------------------------------------------------------
# A small model with every tool in reach uses them when nobody asked ("be idle" -> minimize a window). So each request only
# gets the tools its own words point to; plain chat gets none (and is answered faster).
TRIGGERS = {
    "open_app": r"open|launch|start|run|app|program",
    "open_website": r"open|go to|website|site|\.com|\.org|\.net|youtube|chatgpt|gmail|facebook|url|link|browser",
    "web_search": r"search|google|look ?up",
    "research": r"research|search|look ?up|find out|learn about|information|info|news|latest|summar|about",
    "deep_research": r"deep|in[- ]?depth|detailed|thorough|report|(make|write|create|prepare) (a |me a |an )?(doc|document|paper|summary)|"
                     r"research (on|about|into)|study (on|about)",
    "media_control": r"play|pause|resume|next|previous|skip|song|music|track|video",
    "set_volume": r"volume|louder|quieter|mute|unmute|sound|audio",
    "set_timer": r"timer|remind|alarm|countdown",
    "system_info": r"time is it|what time|the time|date|what day|battery|disk|drive|space|storage|memory|ram|uptime|been on",
    "look_at_screen": r"screen|look at (this|that|it|my|the)|see (this|that|it|my)|read (this|that|it)|this (page|window|tab|error|message)|what('?s| is| does) (this|that)|showing",
    "click_text": r"click|tap|select|button|link|press",
    "type_text": r"type|write|enter|fill|reply|send (a |an |the |my )?(message|text|msg)|(?<!i )(?<!we )(message|text) (him|her|them|to)|"
                 r"ask (it|him|her|them|chatgpt|gemini)|prompt|tell (it|him|her|them)",
    "press_keys": r"press|key|shortcut|enter|send|submit|copy|paste|undo|save|ctrl|alt|escape|ask (it|him|her|them|chatgpt|gemini)|prompt",
    "window_control": r"window|minimi[sz]e|maximi[sz]e|restore|close|quit|exit|switch to|go to|focus|snap|left side|right side|"
                      r"left half|right half|full ?screen|bring (up|it|that|this|back)|to the front|hide",
    "scroll": r"scroll|page down|page up|go down|go up|to the (top|bottom)|further down",
    "find_files": r"find|where|locate|file|folder|search",
    "open_file": r"open|play|watch|file|folder|show",
    "read_file": r"read|file|what'?s in|contents?",
    "write_file": r"file|save|notes?(?!\w)|document|write (it|that|this) down",
    "clipboard": r"clipboard|copy|paste|copied",
    "run_command": r"command|powershell|terminal|cmd|ipconfig|ping|install|uninstall|process|service|script|registry|wifi|network|ip address",
    "send_email": r"e-?mail|mail|gmail",
}


def pick_tools(text, carry=()):
    """The tools for this request: those its words point to, plus the ones of the previous request (for "send it", "yes do it")."""
    t = text.lower()
    names = {n for n, p in TRIGGERS.items() if re.search(rf"\b({p})", t)} | set(carry)
    if "send_email" in names:
        names.add("research")                                                      # "write an email about X" may need to look X up
    if names & {"click_text", "type_text", "press_keys"}:
        names.add("look_at_screen")                                                # acting on the screen means reading it first
    return [tool for tool in TOOLS if tool["function"]["name"] in names]


# ---- arithmetic: small models guess sums ("12 times 8 = 32"), so it is computed here --------------------------------------
def spoken_math(text):
    """'what's 12 times 8' -> ('12 * 8', '96'); None when the sentence holds no calculation."""
    import ast
    import math
    import operator
    t = text.lower().replace(",", "")
    t = re.sub(r"(\d)\s*%\s*of", r"\1 percent of", t)
    for a, b in ((r"square root of\s*(\d+(\.\d+)?)", r"sqrt(\1)"), (r"\bto the power of\b|\braised to\b", "**"), (r"\bsquared\b", "**2"),
                 (r"\bcubed\b", "**3"), (r"\bpercent of\b", "/100*"), (r"\bmultiplied by\b|\btimes\b", "*"), (r"(?<=\d)\s*x\s*(?=\d)", "*"),
                 (r"\bdivided by\b|\bover\b", "/"), (r"\bplus\b", "+"), (r"\bminus\b", "-"), ("×", "*"), ("÷", "/")):
        t = re.sub(a, b, t)
    best = ""
    for m in re.finditer(r"[\d.()+\-*/\s]*(?:sqrt\([\d.]+\))?[\d.()+\-*/\s]*", t):
        chunk = m.group(0).strip()
        if len(re.findall(r"\d+(?:\.\d+)?|sqrt", chunk)) >= 2 and re.search(r"[+\-*/]", chunk) and len(chunk) > len(best):
            best = chunk
    if not best:
        m = re.search(r"sqrt\([\d.]+\)", t)
        best = m.group(0) if m else ""
    if not best:
        return None
    ops = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv, ast.Pow: operator.pow,
           ast.USub: operator.neg, ast.UAdd: operator.pos}

    def ev(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
            return node.value
        if isinstance(node, ast.BinOp) and type(node.op) in ops:
            left, right = ev(node.left), ev(node.right)
            if isinstance(node.op, ast.Pow) and abs(right) > 100:
                raise ValueError("too big")
            return ops[type(node.op)](left, right)
        if isinstance(node, ast.UnaryOp) and type(node.op) in ops:
            return ops[type(node.op)](ev(node.operand))
        if isinstance(node, ast.Call) and getattr(node.func, "id", "") == "sqrt" and len(node.args) == 1:
            return math.sqrt(ev(node.args[0]))
        raise ValueError("not arithmetic")
    try:
        value = ev(ast.parse(best, mode="eval").body)
    except Exception:                                                               # noqa: BLE001
        return None
    shown = str(int(value)) if float(value).is_integer() else f"{value:.6g}"
    return best, shown


# ---- trust ("Always allow") ---------------------------------------------------------------------------------------
def trusted():
    try:
        with open(TRUST_FILE, encoding="utf-8") as f:
            return set(json.load(f).get("always", []))
    except (OSError, ValueError):
        return set()


def trust(name):
    names = trusted() | {name}
    os.makedirs(os.path.dirname(TRUST_FILE), exist_ok=True)
    with open(TRUST_FILE, "w", encoding="utf-8") as f:
        json.dump({"always": sorted(names)}, f)


def forget_trust():
    try:
        os.remove(TRUST_FILE)
    except OSError:
        pass


RUNS_CODE = (".exe", ".bat", ".cmd", ".ps1", ".vbs", ".js", ".jse", ".wsf", ".msi", ".scr", ".com", ".hta", ".reg", ".py")


def needs_approval(name, args):
    level, _ = LEVEL.get(name, ("ask", False))
    if name == "open_file" and str(args.get("path", "")).lower().rstrip('" ').endswith(RUNS_CODE):
        return name not in trusted()                                               # "opening" a program or script runs code: ask
    if level == "auto":
        return False
    if name == "window_control" and args.get("action") == "list":
        return False
    return name not in trusted()


def _foreground_title():
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(user32.GetForegroundWindow(), buf, 256)
    return buf.value or "the desktop"


def describe(name, a):
    """What the approval card says: (action, detail, risky, can always-allow)."""
    can_always = LEVEL.get(name, ("ask", False))[1]
    if name == "open_app":
        return "open an app", a.get("name", ""), False, can_always
    if name == "open_website":
        return "open a website", a.get("url", ""), False, can_always
    if name == "click_text":
        return "click on the screen", f"“{a.get('text', '')}”  in  {_foreground_title()}", False, can_always
    if name == "type_text":
        return f"type into {_foreground_title()[:40]}", a.get("text", ""), False, can_always
    if name == "press_keys":
        return f"press keys in {_foreground_title()[:40]}", a.get("keys", ""), bool(re.search(r"alt\+f4|ctrl\+w|del", a.get("keys", ""), re.I)), can_always
    if name == "window_control":
        return f"{a.get('action', '')} a window", a.get("title", ""), a.get("action") == "close", False
    if name == "find_files":
        return "search your files", a.get("name", "") + (f"  in  {a['folder']}" if a.get("folder") else ""), False, can_always
    if name == "open_file":
        return "open a file", a.get("path", ""), False, can_always
    if name == "read_file":
        return "read a file", a.get("path", ""), False, can_always
    if name == "write_file":
        exists = os.path.exists(_path(a.get("path", "")))
        return (("change a file" if exists else "create a file"), f"{a.get('path', '')}\n\n{a.get('content', '')[:1500]}",
                exists and not a.get("append"), False)
    if name == "clipboard":
        return ("read your clipboard", "", False, can_always) if a.get("action") == "read" else ("copy to the clipboard", a.get("text", ""), False, can_always)
    if name == "send_email":
        to = email_address(a.get("to", ""))
        if not to:
            return "open an email draft (you add the address)", f"Subject: {a.get('subject', '')}\n\n{a.get('body', '')}", False, False
        return f"send an email to {to}", f"Subject: {a.get('subject', '')}\n\n{a.get('body', '')}", False, False
    if name == "run_command":
        cmd = a.get("command", "")
        return "run a command", cmd, bool(RISKY.search(cmd)), False
    return f"use {name}", json.dumps(a), False, False


# ---- apps -----------------------------------------------------------------------------------------------------------
_apps = None
_apps_lock = threading.Lock()


def _load_apps():
    """Every app in the Start menu (desktop programs and Store apps): name -> AppID."""
    global _apps
    with _apps_lock:
        if _apps is not None:
            return _apps
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                                  "Get-StartApps | Select-Object Name, AppID | ConvertTo-Json -Compress"],
                                 capture_output=True, text=True, encoding="utf-8", timeout=30, creationflags=NO_WINDOW).stdout
            items = json.loads(out) if out.strip() else []
            _apps = {i["Name"]: i["AppID"] for i in (items if isinstance(items, list) else [items]) if i.get("Name")}
        except Exception:                                                  # noqa: BLE001
            _apps = {}
        return _apps


def preload():
    threading.Thread(target=_load_apps, daemon=True).start()


ALIASES = {"browser": "chrome", "explorer": "file explorer", "files": "file explorer", "my files": "file explorer", "calc": "calculator",
           "terminal": "terminal", "cmd": "command prompt", "vs code": "visual studio code", "vscode": "visual studio code",
           "word": "word", "excel": "excel", "settings": "settings", "store": "microsoft store", "task manager": "task manager"}


def find_app(name):
    apps = _load_apps()
    want = ALIASES.get(name.lower().strip(), name.lower().strip())
    want = re.sub(r"^(the|my)\s+|\s+app$", "", want)
    names = list(apps)
    exact = [n for n in names if n.lower() == want]
    if exact:
        return exact[0], apps[exact[0]]
    starts = sorted([n for n in names if n.lower().startswith(want)], key=len)
    if starts:
        return starts[0], apps[starts[0]]
    contains = sorted([n for n in names if want in n.lower() and "uninstall" not in n.lower()], key=len)
    if contains:
        return contains[0], apps[contains[0]]
    close = difflib.get_close_matches(want, [n.lower() for n in names], n=1, cutoff=0.6)
    if close:
        n = next(n for n in names if n.lower() == close[0])
        return n, apps[n]
    return None, None


# ---- keyboard ---------------------------------------------------------------------------------------------------------
class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", _KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def _send(inputs):
    arr = (_INPUT * len(inputs))(*inputs)
    user32.SendInput(len(inputs), arr, ctypes.sizeof(_INPUT))


def _key(vk, up=False):
    i = _INPUT(type=1)
    i.ki = _KEYBDINPUT(vk, 0, 2 if up else 0, 0, 0)
    return i


def _tap(vk, times=1):
    for _ in range(times):
        _send([_key(vk), _key(vk, True)])


def type_unicode(text):
    inputs = []
    for ch in text.replace("\r\n", "\n"):
        if ch == "\n":
            inputs += [_key(0x0D), _key(0x0D, True)]
            continue
        code = ord(ch)
        units = [0xD800 + ((code - 0x10000) >> 10), 0xDC00 + ((code - 0x10000) & 0x3FF)] if code > 0xFFFF else [code]   # UTF-16
        for u in units:
            down, up = _INPUT(type=1), _INPUT(type=1)
            down.ki = _KEYBDINPUT(0, u, 4, 0, 0)                                   # KEYEVENTF_UNICODE
            up.ki = _KEYBDINPUT(0, u, 4 | 2, 0, 0)
            inputs += [down, up]
    for i in range(0, len(inputs), 200):
        _send(inputs[i:i + 200])
        time.sleep(0.01)


VK = {"ctrl": 0x11, "control": 0x11, "shift": 0x10, "alt": 0x12, "win": 0x5B, "windows": 0x5B, "enter": 0x0D, "return": 0x0D,
      "tab": 0x09, "esc": 0x1B, "escape": 0x1B, "space": 0x20, "backspace": 0x08, "delete": 0x2E, "del": 0x2E, "insert": 0x2D,
      "home": 0x24, "end": 0x23, "pageup": 0x21, "pagedown": 0x22, "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27,
      "printscreen": 0x2C, "capslock": 0x14, "plus": 0xBB, "minus": 0xBD, "comma": 0xBC, "period": 0xBE}


def press(keys):
    parts = [p.strip().lower() for p in re.split(r"\s*\+\s*", keys) if p.strip()]
    codes = []
    for p in parts:
        if p in VK:
            codes.append(VK[p])
        elif re.fullmatch(r"f([1-9]|1[0-9]|2[0-4])", p):
            codes.append(0x6F + int(p[1:]))
        elif len(p) == 1:
            codes.append(user32.VkKeyScanW(ord(p)) & 0xFF)
        else:
            raise ValueError(f"unknown key '{p}'")
    _send([_key(c) for c in codes] + [_key(c, True) for c in reversed(codes)])


# ---- windows ------------------------------------------------------------------------------------------------------------
def _windows():
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        if user32.IsWindowVisible(hwnd) and user32.GetWindowTextLengthW(hwnd) and not user32.GetWindow(hwnd, 4):   # no owner
            buf = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, buf, 512)
            if buf.value not in ("Program Manager", "Windows Input Experience") and not buf.value.startswith("Eflow"):
                found.append((hwnd, buf.value))
        return True
    user32.EnumWindows(each, 0)
    return found


def _find_window(title):
    wins = _windows()
    t = title.lower()
    for hwnd, name in wins:
        if t in name.lower():
            return hwnd, name
    close = difflib.get_close_matches(t, [n.lower() for _, n in wins], n=1, cutoff=0.4)
    if close:
        return next((h, n) for h, n in wins if n.lower() == close[0])
    return None, None


def _bring_to_front(hwnd):
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)
    _tap(0x12)                                                                      # an Alt tap lets us take the foreground
    user32.SetForegroundWindow(hwnd)


def _path(p):
    """A path the brain gave: ~ and %VARS% expanded, relative ones under the user's folder."""
    p = os.path.expandvars(os.path.expanduser(p.strip().strip('"')))
    return p if os.path.isabs(p) else os.path.join(HOME, p)


# ---- finding files: every drive, by meaning rather than the exact name ----------------------------------------------------
SKIP_DIRS = {"windows", "program files", "program files (x86)", "programdata", "appdata", "$recycle.bin", "system volume information",
             "node_modules", "site-packages", "venv", ".git", "__pycache__", "winsxs", "msocache", "recovery", "$windows.~bt"}
STOP = {"the", "a", "an", "of", "my", "folder", "folders", "file", "files", "named", "called", "in", "on", "for", "and", "movie", "show", "series"}
USER_FOLDERS = ("Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos", "OneDrive")


def _canon(text):
    """Lower-case words, with 'season 1' / 'S01' -> 's1' and 'episode 3' / 'E03' -> 'e3' so they compare equal."""
    t = re.sub(r"[_.\-\[\]()]+", " ", text.lower())
    t = re.sub(r"\bseason\s*0*(\d+)", r"s\1", t)
    t = re.sub(r"\bepisode\s*0*(\d+)", r"e\1", t)
    t = re.sub(r"\bs0*(\d+)\s*e0*(\d+)\b", r"s\1 e\2", t)
    t = re.sub(r"\b([se])0+(\d+)\b", r"\1\2", t)
    return re.findall(r"[a-z0-9]+", t)


def _drives():
    mask, out = kernel32.GetLogicalDrives(), []
    for i in range(26):
        if mask & (1 << i):
            root = f"{chr(65 + i)}:\\"
            if kernel32.GetDriveTypeW(root) in (2, 3):                                # removable or fixed (not network / CD)
                out.append(root)
    return out


def _resolve_folder(folder):
    """'I drive', 'iDrive', 'I:', 'downloads', a full path... -> an existing folder, or None (search everywhere)."""
    if not folder:
        return None
    f = folder.strip().strip('"')
    m = re.fullmatch(r"(?i)([a-z])\s*(:|:\\|\s*-?\s*drive)?\\?", f)
    if m and (m.group(2) or len(f) == 1):
        root = m.group(1).upper() + ":\\"
        return root if os.path.exists(root) else None
    for name in USER_FOLDERS:
        if f.lower() == name.lower():
            return os.path.join(HOME, name)
    p = _path(f)
    return p if os.path.isdir(p) else None


def search_files(name, folder=None, budget=7.0, limit=12):
    words = [w for w in _canon(name) if w not in STOP]
    if not words:
        return "error: say what to look for"
    episodes = [w for w in words if re.fullmatch(r"[se]\d+", w)]
    title = [w for w in words if w not in episodes]
    acronym = "".join(w[0] for w in _canon(name) if not w.isdigit() and not re.fullmatch(r"[se]\d+", w))   # Game Of Thrones -> got
    root = _resolve_folder(folder)
    roots = [root] if root else [os.path.join(HOME, d) for d in USER_FOLDERS if os.path.isdir(os.path.join(HOME, d))] + _drives()
    note = "" if root or not folder else f"(there is no folder '{folder}', so every drive was searched) "

    def score(entry_name, parent):
        tokens = set(_canon(entry_name))
        context = tokens | set(_canon(parent)[-12:])                             # a season folder inside the show's folder counts
        hit = sum(1 for w in title if w in tokens or any(t.startswith(w) and len(w) >= 4 for t in tokens))
        full = bool(title) and hit == len(title)
        short = not full and len(acronym) >= 2 and acronym in context            # "GOT" for Game of Thrones
        if title and not full and not short:
            close = difflib.SequenceMatcher(None, " ".join(title), " ".join(_canon(entry_name))).ratio()
            if close < 0.75 and (len(title) < 3 or hit < len(title) - 1):        # one shared word ("game") is not enough
                return 0
        s = 3 if full else 2 if short else 1
        s += sum(2 for e in episodes if e in context)
        s -= sum(1 for e in episodes if e not in context and any(re.fullmatch(e[0] + r"\d+", t) for t in tokens))   # S07 when S1 was asked
        return s

    found, seen, deadline = [], set(), time.monotonic() + budget
    queue_ = [(r, 0) for r in roots]
    while queue_ and time.monotonic() < deadline:                                 # breadth first: shallow (likely) places first
        nxt = []
        for path, depth in queue_:
            if time.monotonic() > deadline:
                break
            try:
                with os.scandir(path) as it:
                    for e in it:
                        n = e.name
                        is_dir = e.is_dir(follow_symlinks=False)
                        if is_dir and (n.lower() in SKIP_DIRS or n.startswith((".", "$"))):
                            continue
                        s = score(n, path)
                        if s > 0 and e.path.lower() not in seen:
                            seen.add(e.path.lower())
                            found.append((s, is_dir, depth, e.path))
                        if is_dir and depth < 8:
                            nxt.append((e.path, depth + 1))
            except OSError:
                continue
        queue_ = nxt
        if sum(1 for f in found if f[0] >= 3 + 2 * len(episodes)) >= 3:           # plenty of exact matches: no need to dig deeper
            break
    if not found:
        return note + f"nothing related to '{name}' was found"
    found.sort(key=lambda f: (-f[0], not f[1], f[2]))                           # best match, folders first, shallow first
    best = found[0][0]
    lines = [("[folder] " if d else "") + p for s, d, _, p in found[:limit] if s >= best - 2]
    return note + "best matches first:\n" + "\n".join(lines)


# ---- the internet ----------------------------------------------------------------------------------------------------------
UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"}


def _page_text(url, limit=1800):
    """The readable text of a web page (scripts, menus and markup removed), cut to `limit` characters."""
    import html as htmlmod
    import urllib.request
    try:
        raw = urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=6).read(600_000).decode("utf-8", "replace")
    except Exception:                                                               # noqa: BLE001
        return ""
    raw = re.sub(r"(?is)<(script|style|noscript|svg|nav|header|footer|form|aside)\b.*?</\1>", " ", raw)
    paras = [htmlmod.unescape(re.sub(r"<[^>]+>", " ", p)) for p in re.findall(r"(?is)<(?:p|li|h[1-3])\b[^>]*>(.*?)</(?:p|li|h[1-3])>", raw)]
    paras = [re.sub(r"\s+", " ", p).strip() for p in paras]
    text = " ".join(p for p in paras if len(p) > 40)                                # real sentences, not menu items
    return text[:limit]


def search_results(query):
    """Web search (DuckDuckGo, no key): [(title, url, snippet)], ads removed."""
    import html as htmlmod
    import urllib.request
    req = urllib.request.Request("https://html.duckduckgo.com/html/", data=urllib.parse.urlencode({"q": query}).encode(), headers=UA)
    page = urllib.request.urlopen(req, timeout=10).read().decode("utf-8", "replace")
    links = re.findall(r'class="result__a" href="([^"]+)"[^>]*>(.*?)</a>', page)
    snips = re.findall(r'class="result__snippet"[^>]*>(.*?)</a>', page, re.S)
    results = []
    for (u, title), sn in zip(links, snips):
        u = urllib.parse.unquote(re.sub(r".*uddg=([^&]+).*", r"\1", u))
        if "duckduckgo.com/y.js" in u:                                              # an ad
            continue
        results.append((htmlmod.unescape(re.sub("<.*?>", "", title)), u, htmlmod.unescape(re.sub("<.*?>", "", sn)).strip()))
    return results


def research(query, pages=3):
    results = search_results(query)
    if not results:
        return f"no results for '{query}'"
    texts = [""] * pages
    workers = [threading.Thread(target=lambda i=i: texts.__setitem__(i, _page_text(results[i][1])), daemon=True)
               for i in range(min(pages, len(results)))]
    for w in workers:
        w.start()
    for w in workers:
        w.join(8)
    out = [f"Search results for '{query}':"]
    for i, (title, url, snip) in enumerate(results[:6]):
        out.append(f"{i + 1}. {title} ({url}): {snip}")
    for i, text in enumerate(texts):
        if text:
            out.append(f"\nFrom {results[i][1]}:\n{text}")
    return "\n".join(out)[:6000]


def email_address(spoken):
    """'John at gmail dot com' -> 'john@gmail.com'; '' when it isn't a usable address."""
    t = (spoken or "").strip().lower()
    t = re.sub(r"\s+at\s+", "@", t)
    t = re.sub(r"\s+dot\s+", ".", t)
    t = re.sub(r"\s+", "", t).strip(".,;")
    return t if re.fullmatch(r"[a-z0-9._%+\-]+@[a-z0-9\-]+(\.[a-z0-9\-]+)*\.[a-z]{2,}", t) else ""


def gmail_draft(subject, body):
    """Open a Gmail compose window with the subject and text filled in and the address left for the user."""
    os.startfile("https://mail.google.com/mail/?view=cm&fs=1&" + urllib.parse.urlencode({"su": subject, "body": body}, quote_via=urllib.parse.quote))
    return "opened the email as a draft in Gmail; the user types the address and presses Send. Tell them that."


def send_gmail(to, subject, body):
    """Open a filled-in Gmail compose window in the browser and send it (Ctrl+Enter). Needs Gmail signed in there."""
    url = "https://mail.google.com/mail/?view=cm&fs=1&" + urllib.parse.urlencode({"to": to, "su": subject, "body": body},
                                                                                  quote_via=urllib.parse.quote)
    os.startfile(url)
    deadline, title = time.monotonic() + 15, ""
    while time.monotonic() < deadline:                                              # wait for the compose page to be in front
        time.sleep(0.5)
        title = _foreground_title()
        if "gmail" in title.lower() or "compose" in title.lower() or "new message" in title.lower():
            break
    else:
        return f"error: Gmail did not open (the browser shows '{title}'); the email was not sent"
    time.sleep(3.0)                                                                 # the compose form needs a moment
    import screen
    look = screen.look_at_foreground()
    seen = screen.read_text(look).lower()
    if "sign in" in seen and "send" not in seen:
        return "error: Gmail asks to sign in first, so the email was not sent"
    press("ctrl+enter")                                                             # Gmail's "Send"
    for _ in range(12):                                                             # look for Gmail's "Message sent"
        time.sleep(0.5)
        if "message sent" in screen.read_text(screen.look_at_foreground()).lower():
            return f"email sent to {to} (Gmail confirmed 'Message sent')"
    return f"pressed Send in Gmail for the email to {to}, but could not see Gmail confirm it; tell the user to check"


# ---- the work --------------------------------------------------------------------------------------------------------------
class Hands:
    def __init__(self, solu):
        self.solu = solu                                                            # for timers (speaking) and screen help

    def run(self, name, a):
        fn = getattr(self, "do_" + name, None)
        if fn is None:
            return f"error: there is no tool called {name}"
        try:
            return fn(**a)
        except TypeError as e:
            return f"error: wrong arguments ({e})"
        except Exception as e:                                                      # noqa: BLE001
            return f"error: {str(e)[:300]}"

    def do_open_app(self, name):
        found, appid = find_app(name)
        if not found:
            return f"error: no app called '{name}' is installed (check the name)"
        subprocess.Popen(["explorer.exe", f"shell:AppsFolder\\{appid}"], creationflags=NO_WINDOW)
        return f"opened {found}"

    def do_open_website(self, url):
        if not re.match(r"^[a-z]+://", url):
            url = "https://" + url
        os.startfile(url)
        time.sleep(3.5)                                                             # let the page load before anything is typed into it
        return f"opened {url} in the browser; it has loaded and its main text box usually has the cursor"

    def do_web_search(self, query):
        os.startfile("https://www.google.com/search?q=" + urllib.parse.quote_plus(query))
        return f"opened a web search for '{query}' in the browser (the results are on the user's screen, not read by you)"

    def do_research(self, query):
        return research(query)

    def do_deep_research(self, topic, depth="deep"):
        return self.solu.start_research(topic, depth)

    def do_send_email(self, subject, body, to=""):
        address = email_address(to)
        if not address:                                                             # unclear ("google.com"): a draft, never a guess
            return gmail_draft(subject, body)
        return send_gmail(address, subject, body)

    def do_media_control(self, action):
        _tap({"play_pause": 0xB3, "next": 0xB0, "previous": 0xB1, "stop": 0xB2}[action])
        return f"pressed {action.replace('_', '/')}"

    def do_set_volume(self, percent=None, change=None, mute=None):
        if mute is not None:
            _tap(0xAD)                                                              # a toggle: Windows has no separate keys
            return "toggled mute"
        if percent is not None:
            percent = max(0, min(100, int(percent)))
            _tap(0xAE, 50)                                                          # down to 0, then up in 2 % steps
            _tap(0xAF, round(percent / 2))
            return f"volume set to {percent}%"
        if change is not None:
            steps = round(abs(float(change)) / 2)
            _tap(0xAF if change > 0 else 0xAE, steps)
            return f"volume {'up' if change > 0 else 'down'} by {steps * 2}%"
        return "error: say a level, a change or mute"

    def do_set_timer(self, seconds, label=""):
        seconds = float(seconds)
        if seconds <= 0:
            return "error: the timer must be longer than 0 seconds"
        what = f"{label} timer" if label else "timer"

        def ring():
            self.solu.event("timer", label=label)
            threading.Thread(target=self.solu.respond_text, args=(f"Your {what} is done!",), daemon=True).start()
        t = threading.Timer(seconds, ring)
        t.daemon = True
        t.start()
        mins, secs = divmod(int(seconds), 60)
        return f"{what} started for {mins} min {secs} s" if mins else f"{what} started for {secs} s"

    def do_system_info(self):
        class POWER(ctypes.Structure):
            _fields_ = [("ac", ctypes.c_byte), ("flag", ctypes.c_byte), ("percent", ctypes.c_byte), ("saver", ctypes.c_byte),
                        ("life", wintypes.DWORD), ("full", wintypes.DWORD)]
        p = POWER()
        kernel32.GetSystemPowerStatus(ctypes.byref(p))
        battery = "no battery (desktop PC)" if p.flag & 128 or p.percent < 0 or p.percent > 100 else \
            f"battery {p.percent}%{' charging' if p.ac == 1 else ''}"
        kernel32.GetTickCount64.restype = ctypes.c_ulonglong
        up = kernel32.GetTickCount64() // 1000
        class MEM(ctypes.Structure):
            _fields_ = [("len", wintypes.DWORD), ("load", wintypes.DWORD)] + [(n, ctypes.c_ulonglong) for n in
                                                                               ("total", "free", "pt", "pf", "vt", "vf", "ve")]
        m = MEM()
        m.len = ctypes.sizeof(MEM)
        kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        import shutil
        import string
        drives = []
        for letter in string.ascii_uppercase:
            if os.path.exists(f"{letter}:\\"):
                try:
                    u = shutil.disk_usage(f"{letter}:\\")
                    drives.append(f"{letter}: {u.free / 1e9:.0f} GB free of {u.total / 1e9:.0f} GB")
                except OSError:
                    pass
        return (time.strftime("it is %A %d %B %Y, %I:%M %p") + f"; {battery}; PC on for {up // 3600} h {(up % 3600) // 60} min; "
                f"memory {m.load}% used ({m.free / 1e9:.1f} GB free of {m.total / 1e9:.1f} GB); disks: {'; '.join(drives)}")

    def do_look_at_screen(self):
        if not self.solu.screen_enabled:
            return "error: screen help is switched off by the user"
        import screen
        look = screen.look_at_foreground()
        if look.blocked:
            return "error: " + look.blocked
        self.solu.event("looking", title=look.title, app=look.app, scope=look.scope, thumb=screen.thumbnail_b64(look))
        text = screen.read_text(look)
        return f"window '{look.title}' ({look.app}). Visible text:\n{text[:2500]}"

    def do_click_text(self, text):
        import screen
        look = screen.look_at_foreground()
        if look.blocked:
            return "error: " + look.blocked
        hit = screen.find_text(look, text)
        if hit is None:
            return f"error: could not find '{text}' on the screen"
        x, y, seen = hit
        user32.SetCursorPos(int(x), int(y))
        time.sleep(0.05)
        user32.mouse_event(2, 0, 0, 0, 0)
        time.sleep(0.05)
        user32.mouse_event(4, 0, 0, 0, 0)
        return f"clicked '{seen}'"

    def do_type_text(self, text):
        time.sleep(0.2)
        type_unicode(text)
        return f"typed {len(text)} characters"

    def do_press_keys(self, keys):
        press(keys)
        return f"pressed {keys}"

    def do_window_control(self, action, title=""):
        if action == "list":
            return "open windows: " + "; ".join(n for _, n in _windows()[:25])
        if not title.strip() or title.strip().lower() in ("this", "it", "current", "this window", "the current window", "active"):
            hwnd = user32.GetForegroundWindow()                                     # "close this", "snap it left"
            name = _foreground_title()
        else:
            hwnd, name = _find_window(title)
        if not hwnd:
            return f"error: no open window matches '{title}'"
        if action == "focus":
            _bring_to_front(hwnd)
        elif action == "minimize":
            user32.ShowWindow(hwnd, 6)
        elif action == "maximize":
            user32.ShowWindow(hwnd, 3)
            _bring_to_front(hwnd)
        elif action == "restore":
            user32.ShowWindow(hwnd, 9)
            _bring_to_front(hwnd)
        elif action in ("snap_left", "snap_right"):
            _bring_to_front(hwnd)
            if user32.IsZoomed(hwnd):
                user32.ShowWindow(hwnd, 9)                                          # a maximized window snaps from its normal size
            time.sleep(0.15)
            press("win+left" if action == "snap_left" else "win+right")
            time.sleep(0.2)
            press("esc")                                                             # dismiss Snap Assist's "pick the other half"
        elif action == "close":
            user32.PostMessageW(hwnd, 0x0112, 0xF060, 0)                            # WM_SYSCOMMAND SC_CLOSE: like clicking X (Store apps too)
            for _ in range(20):
                time.sleep(0.1)
                if not user32.IsWindow(hwnd) or not user32.IsWindowVisible(hwnd):
                    return f"closed {name}"
            return f"asked {name} to close, but it is still open (it may be asking to save something)"
        return f"{action} done: {name}"

    def do_scroll(self, direction, pages=1):
        if direction in ("top", "bottom"):
            press("ctrl+home" if direction == "top" else "ctrl+end")
            return f"scrolled to the {direction}"
        times = max(1, min(20, int(float(pages or 1))))
        _tap(0x22 if direction == "down" else 0x21, times)                          # Page Down / Page Up: no mouse moves
        return f"scrolled {direction} {times} page(s)"

    def do_find_files(self, name, folder=None):
        return search_files(name, folder)

    def do_open_file(self, path):
        path = _path(path)
        if not os.path.exists(path):
            return f"error: {path} does not exist"
        os.startfile(path)
        return f"opened {path}"

    def do_read_file(self, path):
        path = _path(path)
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read(6000)
        return f"{path}:\n{text}"

    def do_write_file(self, path, content, append=False):
        path = _path(path)
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "a" if append else "w", encoding="utf-8") as f:
            f.write(content)
        return f"{'added to' if append else 'wrote'} {path}"

    def do_clipboard(self, action, text=""):
        if action == "read":
            out = subprocess.run(["powershell", "-NoProfile", "-Command", "Get-Clipboard -Raw"], capture_output=True, text=True,
                                 encoding="utf-8", timeout=10, creationflags=NO_WINDOW).stdout
            return "clipboard: " + (out.strip()[:3000] or "(empty)")
        subprocess.run(["powershell", "-NoProfile", "-Command", "$input | Set-Clipboard"], input=text, text=True, encoding="utf-8",
                       timeout=10, creationflags=NO_WINDOW)
        return "copied to the clipboard"

    def do_run_command(self, command):
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", command], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=60, cwd=HOME, creationflags=NO_WINDOW)
        out = (r.stdout + ("\n" + r.stderr if r.stderr.strip() else "")).strip()
        return f"exit code {r.returncode}\n{out[:2500] or '(no output)'}"
