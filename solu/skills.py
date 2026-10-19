"""Solu's everyday skills: clipboard history, routines, tidying Downloads, the study buddy, lecture notes, the morning briefing,
rare nudges and smart messaging (WhatsApp / Gmail by name). Each piece is small and talks to the engine through `solu`.
"""
import ctypes
import datetime
import difflib
import json
import os
import re
import shutil
import threading
import time
import urllib.request
from ctypes import wintypes

import actions
import research as research_mod

user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
HOME = os.path.expanduser("~")
DATA = os.path.join(os.environ.get("LOCALAPPDATA", "."), "Eflow")


# ---- clipboard history: the last texts you copied, kept in memory only (never written to disk) ------------------------------
def _clipboard_text():
    user32.OpenClipboard.argtypes = [wintypes.HWND]
    user32.GetClipboardData.restype = wintypes.HANDLE
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [wintypes.HANDLE]
    kernel32.GlobalUnlock.argtypes = [wintypes.HANDLE]
    for _ in range(3):
        if user32.OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        return None
    try:
        handle = user32.GetClipboardData(13)                                   # CF_UNICODETEXT
        if not handle:
            return None
        ptr = kernel32.GlobalLock(handle)
        try:
            return ctypes.wstring_at(ptr) if ptr else None
        finally:
            kernel32.GlobalUnlock(handle)
    finally:
        user32.CloseClipboard()


def looks_secret(text):
    """A password or key: one long 'word' mixing cases, digits and symbols. Never read out or shown."""
    t = text.strip()
    if " " in t or not 8 <= len(t) <= 128:
        return bool(re.search(r"(?i)\b(password|passwd|api[_ -]?key|secret|token)\b\s*[:=]", t))
    kinds = sum(bool(re.search(p, t)) for p in (r"[a-z]", r"[A-Z]", r"\d", r"[^\w]"))
    return kinds >= 3 and not re.match(r"^https?://", t)


class ClipboardHistory:
    def __init__(self, keep=25):
        self.items, self.keep, self.seq = [], keep, 0
        threading.Thread(target=self._watch, daemon=True).start()

    def _watch(self):
        while True:
            time.sleep(1.0)
            seq = user32.GetClipboardSequenceNumber()
            if seq == self.seq:
                continue
            self.seq = seq
            text = _clipboard_text()
            if not text or not text.strip():
                continue
            text = text[:5000]
            self.items = [i for i in self.items if i["text"] != text]
            self.items.append({"text": text, "at": time.time()})
            self.items = self.items[-self.keep:]

    def report(self, query=""):
        if not self.items:
            return "nothing has been copied since Solu started"
        rows = []
        for i in reversed(self.items):
            text = "[hidden: looks like a password or key]" if looks_secret(i["text"]) else i["text"][:300].replace("\n", " ")
            ago = int((time.time() - i["at"]) / 60)
            rows.append(f"({ago} min ago) {text}")
        if query:
            want = query.lower()
            matched = [r for r in rows if want in r.lower()] or rows
            rows = matched
        return "copied recently, newest first:\n" + "\n".join(rows[:12])


# ---- routines: "when I say 'study mode', do ..." -------------------------------------------------------------------------------
ROUTINES = os.path.join(DATA, "solu_routines.json")


class Routines:
    def __init__(self):
        try:
            with open(ROUTINES, encoding="utf-8") as f:
                self.items = json.load(f)
        except (OSError, ValueError):
            self.items = {}

    def _save(self):
        os.makedirs(DATA, exist_ok=True)
        with open(ROUTINES, "w", encoding="utf-8") as f:
            json.dump(self.items, f, ensure_ascii=False, indent=1)

    def define(self, text):
        """'when I say study mode, mute discord and play lo-fi' -> ('study mode', steps); None if it isn't a definition."""
        m = re.match(r"(?i)^(?:solu,?\s*)?(?:when(?:ever)? i say|if i say|create a routine called|make a routine called)\s+['\"“]?(.+?)['\"”]?\s*"
                     r"(?:,|:|\bthen\b|\byou should\b|\bplease\b|\bit should\b)\s*(?:then\s+|you should\s+|please\s+|do\s+|it should\s+)?(.{6,})$",
                     text.strip())
        if not m:
            return None
        name, steps = m.group(1).strip().lower().rstrip(".,"), m.group(2).strip()
        self.items[name] = steps
        self._save()
        return name, steps

    def match(self, text):
        t = re.sub(r"[^a-z0-9 ]", "", text.lower()).strip()
        t = re.sub(r"^(solu |start |run |do |turn on |switch to |activate )", "", t)
        for name in self.items:
            if t == name or t in (f"{name} please", f"{name} now") or difflib.SequenceMatcher(None, t, name).ratio() > 0.88:
                return name
        return None

    def delete(self, name):
        found = difflib.get_close_matches(name.lower(), list(self.items), n=1, cutoff=0.6)
        if found:
            del self.items[found[0]]
            self._save()
            return found[0]
        return None


# ---- tidy a folder ---------------------------------------------------------------------------------------------------------------
GROUPS = {"Images": ".jpg .jpeg .png .gif .webp .bmp .heic .svg .ico .tif .tiff",
          "Documents": ".pdf .doc .docx .txt .rtf .odt .xls .xlsx .csv .ppt .pptx .md .epub",
          "Videos": ".mp4 .mkv .mov .avi .webm .wmv .m4v",
          "Music": ".mp3 .wav .flac .m4a .aac .ogg .wma",
          "Archives": ".zip .rar .7z .tar .gz .bz2 .xz .iso",
          "Installers": ".exe .msi .msix .appx .msixbundle",
          "Code": ".py .js .ts .html .css .json .cpp .c .h .java .cs .ipynb .sql .sh .ps1 .bat"}
EXT_GROUP = {ext: group for group, exts in GROUPS.items() for ext in exts.split()}
BUSY = (".crdownload", ".part", ".partial", ".tmp", ".download")


def tidy_plan(folder):
    """{group: [file names]} for the loose files in `folder` (folders, downloads in progress and fresh files are left alone)."""
    plan, now = {}, time.time()
    for entry in os.scandir(folder):
        if not entry.is_file() or entry.name.lower().endswith(BUSY) or entry.name.startswith((".", "~$")) or entry.name == "desktop.ini":
            continue
        if now - entry.stat().st_mtime < 120:                                     # just arrived: maybe still in use
            continue
        group = EXT_GROUP.get(os.path.splitext(entry.name)[1].lower(), "Other")
        plan.setdefault(group, []).append(entry.name)
    return plan


def tidy_apply(folder, plan):
    moved = 0
    for group, names in plan.items():
        target = os.path.join(folder, group)
        os.makedirs(target, exist_ok=True)
        for name in names:
            src, dst = os.path.join(folder, name), os.path.join(target, name)
            base, ext = os.path.splitext(name)
            n = 2
            while os.path.exists(dst):                                            # never overwrite: "file (2).pdf"
                dst = os.path.join(target, f"{base} ({n}){ext}")
                n += 1
            try:
                shutil.move(src, dst)
                moved += 1
            except OSError:
                pass
    return moved


# ---- study buddy -------------------------------------------------------------------------------------------------------------
QUIZ = ("Make a spoken quiz from this study material. Write {n} questions that test real understanding of the most important points "
        "(not trivia about formatting), each answerable in one short sentence, with the correct answer.\n"
        'Return JSON: {{"topic": "what the material is about, in a few words", "questions": [{{"q": "...", "a": "...", "point": '
        '"the idea it tests, 2-4 words"}}]}}\n\nMaterial:\n{text}')
GRADE = ("Question: {q}\nCorrect answer: {a}\nStudent's spoken answer (from speech recognition, may have small errors): {answer}\n"
         "Is the student's answer essentially correct? Be fair: accept other wording and partly complete answers that get the key idea.\n"
         'Return JSON: {{"correct": true or false, "feedback": "one short, warm spoken sentence; if wrong, give the right answer"}}')


def pdf_text(path, limit=12000):
    from pypdf import PdfReader
    reader = PdfReader(path)
    out = []
    for page in reader.pages[:30]:
        out.append(page.extract_text() or "")
        if sum(len(p) for p in out) > limit:
            break
    return "\n".join(out)[:limit]


class Quiz:
    def __init__(self, questions, topic):
        self.questions, self.topic = questions, topic
        self.index, self.score, self.missed = 0, 0, []

    @property
    def current(self):
        return self.questions[self.index] if self.index < len(self.questions) else None


# ---- lecture / meeting notes: what the PC plays (not the microphone), transcribed as it goes -----------------------------------
NOTES_FOLDER = os.path.join(HOME, "Documents", "Solu Notes")
NOTES_PART = ("Here is part {i} of a transcript of a {kind} (speech recognition, may contain mistakes):\n{text}\n\n"
              "Write concise notes for this part: the main points as bullets, with names, numbers, definitions and examples kept exact. "
              "Bullets only.")
NOTES_FINAL = ("Write study notes for a {kind} titled \"{title}\" from these notes of its parts:\n{notes}\n\n"
               "Markdown:\n## Summary\n(3-5 sentences)\n## Key points\n(bullets)\nthen 1-4 '## ' sections that organise the content "
               "(definitions, steps, examples)\n## Action items\n(only if any were mentioned: tasks, deadlines, homework; otherwise skip)\n"
               "## Questions to review\n(3-5 questions to test yourself)\nUse only what is in the notes.")


def _soundcard():
    """soundcard 0.4.5 still calls numpy.fromstring (removed in numpy 2): give that one module a shim."""
    import numpy
    import soundcard
    import soundcard.mediafoundation as mf

    class _Numpy:
        def __getattr__(self, name):
            return getattr(numpy, name)

        @staticmethod
        def fromstring(data, dtype=float):
            return numpy.frombuffer(data, dtype=dtype).copy()
    if not isinstance(mf.numpy, _Numpy.__class__) and getattr(mf.numpy, "fromstring", None) is not _Numpy.fromstring:
        mf.numpy = _Numpy()
    return soundcard


class NoteTaker(threading.Thread):
    def __init__(self, solu, kind="lecture"):
        super().__init__(daemon=True)
        self.solu, self.kind = solu, kind
        self.stop_flag = threading.Event()
        self.parts, self.started, self.error = [], time.time(), None

    def run(self):
        import numpy as np
        sc = _soundcard()
        try:
            speaker = sc.default_speaker()
            loop = sc.get_microphone(id=str(speaker.name), include_loopback=True)
            with loop.recorder(samplerate=16000, channels=1, blocksize=1600) as rec:
                buf = []
                while not self.stop_flag.is_set():
                    buf.append(rec.record(numframes=16000)[:, 0].astype(np.float32))       # one second
                    if len(buf) >= 30:                                                     # every 30 s: transcribe it
                        self._transcribe(np.concatenate(buf))
                        buf = []
                if buf:
                    self._transcribe(np.concatenate(buf))
        except Exception as e:                                                             # noqa: BLE001
            self.error = str(e)[:150]
            print(f"   [notes] recording failed: {e}")

    def _transcribe(self, audio):
        import numpy as np
        if float(np.sqrt(np.mean(audio ** 2))) < 0.003:                                    # silence
            return
        text = self.solu.transcribe_long(audio)
        if text:
            self.parts.append(text)
        minutes = int((time.time() - self.started) / 60)
        self.solu.event("activity", id="notes", title="Taking notes", detail=f"{self.kind} · {minutes} min · {len(' '.join(self.parts).split())} words")

    def finish(self):
        """Stop recording and write the notes page. Returns the .html path (or raises)."""
        self.stop_flag.set()
        self.join(40)
        transcript = " ".join(self.parts).strip()
        if len(transcript.split()) < 30:
            raise RuntimeError("I didn't hear enough speech to take notes" + (f" ({self.error})" if self.error else ""))
        chunks, words = [], transcript.split()
        for i in range(0, len(words), 1100):                                               # ~6 minutes of speech each
            chunks.append(" ".join(words[i:i + 1100]))
        notes = []
        for i, chunk in enumerate(chunks, 1):
            self.solu.event("activity", id="notes", title="Writing notes", detail=f"part {i} of {len(chunks)}", progress=i / (len(chunks) + 1))
            notes.append(self.solu._ask(NOTES_PART.format(i=i, kind=self.kind, text=chunk)).strip())
        title = self.solu._ask(f"Give a short title (max 8 words) for a {self.kind} with these notes. Reply with the title only.\n"
                               + "\n".join(notes)[:3000]).strip().strip('"')[:80]
        markdown = self.solu._ask(NOTES_FINAL.format(kind=self.kind, title=title, notes="\n\n".join(notes))).strip()
        markdown = re.sub(r"^#\s+[^\n]*\n+", "", re.sub(r"^```(?:markdown)?|```$", "", markdown).strip()).strip()
        os.makedirs(NOTES_FOLDER, exist_ok=True)
        stamp = datetime.datetime.now()
        base = os.path.join(NOTES_FOLDER, f"{stamp:%Y-%m-%d %H%M} {research_mod._slug(title)}")
        minutes = max(1, round((time.time() - self.started) / 60))
        page = research_mod.to_html(title, title, markdown, [], f"{stamp:%d %B %Y}  ·  {self.kind.capitalize()} notes  ·  {minutes} min recorded")
        page = page.replace('<div class="kicker">Solu research</div>', '<div class="kicker">Solu notes</div>')
        page = page.replace('<section class="sources"><h2>Sources</h2><ol></ol></section>', "")
        page = page.replace("Researched and written by Solu on this PC from 0 web sources. Check important facts in the sources.",
                            "Transcribed and written by Solu on this PC. Speech recognition can mishear names and numbers.")
        with open(base + ".html", "w", encoding="utf-8") as f:
            f.write(page)
        with open(base + ".md", "w", encoding="utf-8") as f:
            f.write(f"# {title}\n\n{markdown}\n\n## Transcript\n{transcript}\n")
        research_mod.open_report(base + ".html")
        return base + ".html", title, markdown


# ---- the morning briefing -----------------------------------------------------------------------------------------------------
def weather():
    """Today's weather where this PC is (wttr.in finds the place from the internet connection; no key)."""
    try:
        req = urllib.request.Request("https://wttr.in/?format=j1", headers={"User-Agent": "curl/8"})
        data = json.loads(urllib.request.urlopen(req, timeout=6).read())
        now = data["current_condition"][0]
        today = data["weather"][0]
        place = data.get("nearest_area", [{}])[0].get("areaName", [{}])[0].get("value", "")
        return (f"{place}: now {now['temp_C']} C and {now['weatherDesc'][0]['value'].lower()}, "
                f"today {today['mintempC']} to {today['maxtempC']} C, chance of rain {max(int(h.get('chanceofrain', 0)) for h in today['hourly'])}%")
    except Exception:                                                              # noqa: BLE001
        return ""


def unread_summary(context):
    items = (context or {}).get("notifications", [])
    if not items:
        return ""
    by_app = {}
    for n in items:
        by_app.setdefault(n.get("app") or "an app", []).append(n.get("title", ""))
    parts = []
    for app, titles in sorted(by_app.items(), key=lambda kv: -len(kv[1])):
        senders = [t for t in dict.fromkeys(titles) if t][:3]
        parts.append(f"{len(titles)} from {app}" + (f" ({', '.join(senders)})" if senders else ""))
    return "; ".join(parts[:4])


# ---- nudges: rare, quiet, dismissible -------------------------------------------------------------------------------------------
class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]


def idle_seconds():
    info = LASTINPUTINFO(ctypes.sizeof(LASTINPUTINFO), 0)
    user32.GetLastInputInfo(ctypes.byref(info))
    return (kernel32.GetTickCount() - info.dwTime) / 1000.0


MONTHS = {m.lower(): i for i, m in enumerate(["January", "February", "March", "April", "May", "June", "July", "August",
                                                "September", "October", "November", "December"], 1)}


def dates_in(text):
    """Dates written in a memory ('my exam is on October 20th', 'on 20 October', '2026-10-20')."""
    out, year = [], datetime.date.today().year
    for m in re.finditer(r"(?i)\b(january|february|march|april|may|june|july|august|september|october|november|december)\s+(\d{1,2})", text):
        out.append((MONTHS[m.group(1).lower()], int(m.group(2))))
    for m in re.finditer(r"(?i)\b(\d{1,2})(?:st|nd|rd|th)?\s+(?:of\s+)?(january|february|march|april|may|june|july|august|september|october|november|december)", text):
        out.append((MONTHS[m.group(2).lower()], int(m.group(1))))
    dates = []
    for month, day in out:
        try:
            dates.append(datetime.date(year, month, day))
        except ValueError:
            pass
    for m in re.finditer(r"\b(20\d\d)-(\d\d)-(\d\d)\b", text):
        try:
            dates.append(datetime.date(int(m.group(1)), int(m.group(2)), int(m.group(3))))
        except ValueError:
            pass
    return dates


class Nudges(threading.Thread):
    """Looks once a minute for something worth a quiet pill: low battery, a plan for tomorrow, two hours without a break."""

    def __init__(self, solu):
        super().__init__(daemon=True)
        self.solu, self.last, self.sent_today, self.day = solu, 0.0, 0, datetime.date.today()
        self.active_since, self.done = None, set()

    def nudge(self, key, title, text):
        if key in self.done or time.time() - self.last < 1800 or self.sent_today >= 6 or getattr(self.solu, "paused", False):
            return
        self.done.add(key)
        self.last = time.time()
        self.sent_today += 1
        self.solu.event("nudge", title=title, text=text)
        print(f"   [nudge] {title}: {text}")

    def run(self):
        time.sleep(90)
        while True:
            try:
                self.check()
            except Exception as e:                                                # noqa: BLE001
                print(f"   (nudge check failed: {e})")
            time.sleep(60)

    def check(self):
        today = datetime.date.today()
        if today != self.day:
            self.day, self.sent_today = today, 0
            self.done = {k for k in self.done if not k.startswith("plan:")}
        # Battery
        class POWER(ctypes.Structure):
            _fields_ = [("ac", ctypes.c_byte), ("flag", ctypes.c_byte), ("percent", ctypes.c_byte), ("saver", ctypes.c_byte),
                        ("life", wintypes.DWORD), ("full", wintypes.DWORD)]
        p = POWER()
        kernel32.GetSystemPowerStatus(ctypes.byref(p))
        if p.ac == 0 and 0 <= p.percent <= 15 and not p.flag & 128:
            self.nudge(f"battery:{today}:{p.percent // 5}", "Battery low", f"{p.percent}% left. Time to plug in?")
        # Plans for tomorrow (from memory)
        tomorrow = today + datetime.timedelta(days=1)
        for m in self.solu.memory.items:
            if tomorrow in dates_in(m["text"]):
                from memory import spoken
                self.nudge(f"plan:{tomorrow}:{m['text'][:40]}", "Tomorrow", spoken(m["text"]))
        # Two hours at the PC without a real break
        idle = idle_seconds()
        if idle > 300:
            self.active_since = None
        elif self.active_since is None:
            self.active_since = time.time()
        elif time.time() - self.active_since > 2 * 3600:
            self.nudge(f"break:{int(self.active_since)}", "Break?", "Two hours straight. A short stretch might help.")


# ---- smart messaging ---------------------------------------------------------------------------------------------------------------
def whatsapp_open_chat(name):
    """Open WhatsApp, search the contact, open the chat and check its header. Returns (chat title or None, note)."""
    import screen
    found, appid = actions.find_app("WhatsApp")
    if not found:
        return None, "WhatsApp isn't installed"
    import subprocess
    subprocess.Popen(["explorer.exe", f"shell:AppsFolder\\{appid}"], creationflags=0x08000000)
    for _ in range(20):                                                            # wait for it to be in front
        time.sleep(0.4)
        if "whatsapp" in actions._foreground_title().lower():
            break
    else:
        return None, "WhatsApp didn't come to the front"
    time.sleep(0.8)
    look = screen.look_at_foreground()
    box = screen.find_text(look, "Search or start a new chat") or screen.find_text(look, "Search")
    if box:
        user32.SetCursorPos(int(box[0]), int(box[1]))
        user32.mouse_event(2, 0, 0, 0, 0)
        user32.mouse_event(4, 0, 0, 0, 0)
    else:
        actions.press("ctrl+f")
    time.sleep(0.4)
    actions.press("ctrl+a")
    actions.type_unicode(name)
    time.sleep(1.4)                                                                # results appear
    import asyncio
    import io as iomod

    def ocr_lines(look):
        buf = iomod.BytesIO()
        look.image.save(buf, "PNG")
        out = []
        for words in asyncio.run(screen._ocr_words(buf.getvalue())):
            if words:
                x0, y0 = min(w[1] for w in words), min(w[2] for w in words)
                x1, y1 = max(w[1] + w[3] for w in words), max(w[2] + w[4] for w in words)
                out.append((" ".join(w[0] for w in words), x0, y0, x1, y1))
        return out

    want = re.sub(r"[^a-z0-9]", "", name.lower())

    def similarity(line):
        norm = re.sub(r"[^a-z0-9]", "", line.lower())
        return 0.9 if want and want in norm else difflib.SequenceMatcher(None, want, norm).ratio()

    # Click the best search result in the chat list (left column, below the search box); Enter does not open one.
    look = screen.look_at_foreground()
    width, height = look.image.size
    results = [ln for ln in ocr_lines(look) if ln[1] < width * 0.35 and ln[2] > height * 0.18]
    if not results:
        return None, f"WhatsApp found no chat for {name}"
    best = max(results, key=lambda ln: similarity(ln[0]))
    if similarity(best[0]) < 0.6:
        return None, f"I couldn't find a chat called {name} in WhatsApp"
    user32.SetCursorPos(int(look.origin[0] + (best[1] + best[3]) / 2), int(look.origin[1] + (best[2] + best[4]) / 2))
    user32.mouse_event(2, 0, 0, 0, 0)
    user32.mouse_event(4, 0, 0, 0, 0)
    time.sleep(1.2)
    # Check: the open chat's name is in the header of the conversation pane (right side, top), not in the search box on the
    # left, which still shows what was typed. Only that header counts.
    look = screen.look_at_foreground()
    width, height = look.image.size
    strip = look.image.crop((int(width * 0.2), 0, int(width * 0.8), int(height * 0.12)))   # the header, enlarged: thin text reads better
    strip = strip.resize((strip.width * 2, strip.height * 2))
    header = [ln[0] for ln in ocr_lines(type("L", (), {"image": strip})())]
    if header:
        top = max(header, key=similarity)
        if similarity(top) >= 0.6:
            return top, ""
        return None, f"I opened a chat, but it is “{top}”, not {name}, so I stopped"
    return best[0], ""                                                             # header unreadable: the result that was clicked


def whatsapp_type_and_send(message):
    actions.type_unicode(message)
    time.sleep(0.3)
    actions.press("enter")
    return "sent"


def gmail_lookup(name):
    """Open a Gmail compose window, type the name in To and read Gmail's suggestion. Returns (address or None, note)."""
    import screen
    os.startfile("https://mail.google.com/mail/?view=cm&fs=1")
    for _ in range(30):
        time.sleep(0.5)
        if "gmail" in actions._foreground_title().lower() or "compose" in actions._foreground_title().lower():
            break
    else:
        return None, "Gmail didn't open in the browser"
    time.sleep(3.0)
    actions.type_unicode(name)                                                    # the To field has the cursor in a new compose
    time.sleep(1.8)
    text = screen.read_text(screen.look_at_foreground())
    found = re.findall(r"[A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)*\.[A-Za-z]{2,}", text)
    found = [f for f in dict.fromkeys(found) if not f.lower().startswith(("noreply", "no-reply"))]
    if not found:
        return None, f"Gmail didn't suggest an address for {name}"
    return found[0], ""
