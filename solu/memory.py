"""Solu's long-term memory: a few lasting facts about the user, kept on this PC.

Nothing is saved sentence by sentence. Facts are added when the user says "remember ...", or picked out once, in the background,
after a conversation has ended (name, people and how to reach them, preferences, plans). Recall is a fast word match: each
request gets only the memories that relate to it, plus the core ones (the user's name and preferences).
File: %LOCALAPPDATA%\\Eflow\\solu_memory.json   (readable; delete it to wipe the memory)
"""
import difflib
import json
import os
import re
import shutil
import threading
import time

PATH = os.path.join(os.environ.get("LOCALAPPDATA", "."), "Eflow", "solu_memory.json")
KINDS = ("name", "person", "preference", "plan", "fact")
CORE = ("name", "preference")                                   # always in mind, whatever the topic
LIMIT = 400
STOP = set("""a an the and or but if so to of in on at for with from by about as is are was were be been being am do does did
have has had i me my mine you your yours it its this that these those he she they them his her their we our us what which who
whom when where why how not no yes can could will would should may might must just very really also too than then there here
please solu hey okay ok like want need get got make made tell said say says user user's""".split())

EXTRACT = (
    "You maintain a small long-term memory about the user of a desktop assistant. Read the conversation below and pick out only "
    "lasting facts worth remembering in future conversations: people in their life (relation, and how to reach "
    "them: phone, email, WhatsApp), their preferences and dislikes, ongoing plans or goals, and important personal details. "
    "Preferences about how the assistant should behave count too (e.g. 'The user prefers short answers.'). "
    "Only facts the USER said about themselves, in their own words. Never facts from their screen, web pages, documents or "
    "accounts, never the user's name (it is saved separately), and never anything that sounds misheard or does not make sense. "
    "Skip small talk, questions, one-off requests and commands, things the assistant said, and anything already in the memory. "
    "Write each fact as one short sentence about 'the user' (e.g. 'The user's sister is Shoily; they message her on WhatsApp.'). "
    'Return JSON: {"memories": [{"text": "...", "kind": "name|person|preference|plan|fact"}]} with at most 3 items; '
    'usually there is nothing to save: then return {"memories": []}.'
)


def spoken(fact):
    """'The user's sister is Shoily' -> 'Your sister is Shoily.' (memories are stored about 'the user', read back to 'you')."""
    told = re.match(r"(?i)the user told solu:\s*(.+)", fact)
    if told:
        said = told.group(1)
        for a, b in ((r"\bmy\b", "your"), (r"\bI'm\b", "you're"), (r"\bI am\b", "you are"), (r"\bI\b", "you"), (r"\bme\b", "you"), (r"\bmine\b", "yours")):
            said = re.sub(a, b, said, flags=re.I)
        fact = "You told me " + said
    irregular = {"has": "have", "is": "are", "was": "were", "does": "do", "goes": "go"}
    fact = re.sub(r"\b([Tt])he user (\w+)\b",                                       # "The user prefers" -> "You prefer"
                  lambda m: ("You " if m.group(1) == "T" else "you ") + irregular.get(m.group(2), re.sub(r"(?<=[^s])s$", "", m.group(2))
                                                                                 if m.group(2).endswith("s") and m.group(2) not in ("is", "was") else m.group(2)), fact)
    for a, b in ((r"\bThe user's\b", "Your"), (r"\bthe user's\b", "your"), (r"\bThe user\b", "You"), (r"\bthe user\b", "you"),
                 (r"\bthey message\b", "you message"), (r"\bthey\b(?= (like|prefer|want|use|live|work|study))", "you")):
        fact = re.sub(a, b, fact)
    return fact.rstrip(".") + "."


def _words(text):
    out = set()
    for w in re.findall(r"[a-z0-9@.+]+", text.lower()):
        w = w.strip(".")
        if len(w) < 2 or w in STOP:
            continue
        out.add(w[:-1] if len(w) > 4 and w.endswith("s") and not w.endswith("ss") else w)   # sisters -> sister
    return out


PEOPLE = r"(?:best friend|friend|sister|brother|mom|mum|mother|dad|father|wife|husband|girlfriend|boyfriend|partner|son|daughter|boss|teacher|cousin)"


def from_statement(text):
    """A plain statement worth keeping the moment it is said: 'my favourite playlist is maths', 'my sister's name is Mim',
    'my favorite playlist on Spotify, which is Maths'. Returns (fact, kind) or None. Questions are never facts."""
    t = re.sub(r"\s+", " ", text).strip()
    if t.endswith("?") or re.match(r"(?i)^(what|who|when|where|which|how|why|do|does|did|is|are|can|could)\b", t):
        return None
    m = re.search(r"(?i)\bmy ((?:favou?rite|usual) [\w' ]{2,30}?)(?: on \w+)?,? (?:which|that) (?:is|are) ([^,.!?]{1,50})", t) or \
        re.search(r"(?i)\bmy ((?:favou?rite|usual) [\w' ]{2,30}?)(?: on \w+)? (?:is|are) ([^,.!?]{1,50})", t) or \
        re.search(rf"(?i)\bmy ({PEOPLE}(?:'s| s)? (?:name|birthday|number|email|favou?rite [\w ]+?))(?: is| are|'s) ([^,.!?]{{1,50}})", t) or \
        re.search(rf"(?i)\bmy ({PEOPLE})(?:'s| is) (?:name is |called )?([A-Z][\w'-]+)", t)
    if not m:
        return None
    what, value = m.group(1).strip().lower().replace("favourite", "favorite"), m.group(2).strip()
    if re.match(r"(?i)^(not|a|the|so|very|really|too)\b", value) or len(value) < 2:
        return None
    if re.fullmatch(PEOPLE, what):
        what += "'s name"
    kind = "preference" if what.startswith(("favorite", "usual")) else "person"
    return f"The user's {what} is {value}.", kind


class Memory:
    def __init__(self, path=PATH):
        self.path = path
        self.lock = threading.Lock()
        self.items = []
        self._mtime = None                                         # the file as this process last read or wrote it
        self._load()

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as f:
                self.items = [m for m in json.load(f).get("memories", []) if m.get("text")]
            self._mtime = os.path.getmtime(self.path)
        except (OSError, ValueError):
            pass

    def _fresh(self):
        """Re-read the file if someone else changed it (another Solu, or the user cleaning it): saving the old copy from
        this process's memory would bring deleted facts back."""
        try:
            if os.path.getmtime(self.path) != self._mtime:
                self._load()
        except OSError:
            pass

    def _save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"memories": self.items}, f, ensure_ascii=False, indent=1)
        os.replace(tmp, self.path)                                 # never a half-written file
        self._mtime = os.path.getmtime(self.path)

    def _similar(self, a, b):
        wa, wb = _words(a), _words(b)
        overlap = len(wa & wb) / max(1, min(len(wa), len(wb)))
        return overlap >= 0.75 or difflib.SequenceMatcher(None, a.lower(), b.lower()).ratio() > 0.85

    def add(self, text, kind="fact", source="told"):
        """Save a fact; a fact that says the same thing (or a newer name) replaces the old one. Returns True if anything changed."""
        text = re.sub(r"\s+", " ", text).strip()
        if len(text) < 6:
            return False
        kind = kind if kind in KINDS else "fact"
        with self.lock:
            self._fresh()
            if kind == "name" and source != "told" and any(m["kind"] == "name" and m.get("source") == "told" for m in self.items):
                return False                                        # a guess from a misheard conversation ("Yush") never replaces it
            for m in self.items:
                if (kind == "name" and m["kind"] == "name") or self._similar(m["text"], text):
                    if m["text"] == text:
                        return False
                    m.update(text=text, kind=kind, updated=time.strftime("%Y-%m-%d"))
                    self._save()
                    return True
            self.items.append({"text": text, "kind": kind, "source": source, "created": time.strftime("%Y-%m-%d"), "used": 0})
            if len(self.items) > LIMIT:                             # forget the least useful old facts first
                self.items.sort(key=lambda m: (m["kind"] in CORE, m.get("used", 0), m.get("created", "")))
                self.items = self.items[len(self.items) - LIMIT:]
            self._save()
            return True

    def relevant(self, text, k=5):
        """The memories that matter for this request: the core ones, then the best word matches. Microseconds."""
        with self.lock:
            self._fresh()
            if not self.items:
                return []
            core = [m for m in self.items if m["kind"] in CORE][:4]
            want = _words(text)
            scored = []
            for m in self.items:
                if m in core:
                    continue
                hit = len(want & _words(m["text"]))
                if hit:
                    scored.append((hit, m))
            scored.sort(key=lambda s: -s[0])
            picked = core + [m for _, m in scored[:k]]
            for m in picked:
                m["used"] = m.get("used", 0) + 1                    # (counts are saved with the next change)
            return [m["text"] for m in picked]

    def user_name(self):
        """The user's name as they told it ('Eusha Ibna Akbor'), or None."""
        with self.lock:
            self._fresh()
            for m in reversed(self.items):
                hit = m["kind"] == "name" and re.search(r"(?i)name is (.+?)\.?$", m["text"])
                if hit:
                    return hit.group(1).strip()
        return None

    def find_favorite(self, what):
        """'playlist' -> 'maths' when the user said their favourite playlist is maths; None if unknown."""
        with self.lock:
            self._fresh()
            for m in reversed(self.items):
                hit = re.search(rf"favou?rite {what}\w*(?: on \w+)? (?:is|are) (.+?)\.?$", m["text"], re.I)
                if hit:
                    return hit.group(1).strip()
        return None

    def forget(self, query):
        """Remove the memory that best matches `query`; returns its text, or None."""
        want = _words(query)
        with self.lock:
            self._fresh()
            best, score = None, 0.0
            for m in self.items:
                words = _words(m["text"])
                s = len(want & words) / max(1, len(want))
                s = max(s, difflib.SequenceMatcher(None, query.lower(), m["text"].lower()).ratio())
                if s > score:
                    best, score = m, s
            if best is None or score < 0.5:
                return None
            self.items.remove(best)
            self._save()
            return best["text"]

    def clear(self):
        with self.lock:
            self._fresh()
            if os.path.exists(self.path):
                shutil.copyfile(self.path, self.path + ".bak")      # one undo, just in case
            self.items = []
            self._save()

    def summary(self, n=8):
        with self.lock:
            self._fresh()
            ranked = sorted(self.items, key=lambda m: (m["kind"] not in CORE, -m.get("used", 0)))
            return [m["text"] for m in ranked[:n]], len(self.items)

    def learn_from(self, transcript, ask_brain):
        """After a conversation: let the brain pick out (rarely more than one) lasting fact. Runs in the background."""
        user_words = sum(len(line.split()) for role, line in transcript if role == "user")
        if user_words < 6:                                           # "hi", "thanks", "what time is it": nothing to learn
            return []
        known = "\n".join(f"- {m['text']}" for m in self.items[-40:]) or "(empty)"
        convo = "\n".join(f"{'User' if role == 'user' else 'Assistant'}: {line[:400]}" for role, line in transcript[-16:])
        try:
            answer = ask_brain(f"{EXTRACT}\n\nAlready in the memory:\n{known}\n\nConversation:\n{convo}")
            found = json.loads(answer).get("memories", [])
        except Exception:                                            # noqa: BLE001  (no brain, bad JSON: just learn nothing)
            return []
        saved = []
        for m in found[:3]:
            if not isinstance(m, dict) or not isinstance(m.get("text"), str) or m.get("kind") == "name":
                continue                                             # names only come from "my name is ...": guesses were misheard
            if self.add(m["text"], m.get("kind", "fact"), "learned"):
                saved.append(m["text"])
        return saved
