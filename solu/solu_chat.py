"""Solu voice prototype: mic -> VAD turn-taking -> Whisper -> Ollama (streamed) -> Kokoro -> speakers.

  python solu_chat.py                  live, headphones (you can interrupt Solu by talking)
  python solu_chat.py --headphones     live, headphones: you can interrupt Solu by talking over it
  (default is speaker mode: Solu does not listen while it talks, so it can never hear its own voice)
  python solu_chat.py --wav in.wav     feed a recording instead of the mic and save the reply (latency test)
Prints, for every turn, how long it took from the moment you stopped talking to the first sound.
"""
import argparse
import ctypes
import difflib
import glob
import json
import os
import queue
import re
import sys
import threading
import time
import urllib.request
import wave

import numpy as np

site = os.path.join(os.path.dirname(sys.executable), "..", "Lib", "site-packages", "nvidia")
for d in glob.glob(os.path.join(site, "*", "bin")):
    os.add_dll_directory(os.path.abspath(d))
    os.environ["PATH"] = os.path.abspath(d) + os.pathsep + os.environ["PATH"]

import onnxruntime as ort  # noqa: E402
from faster_whisper import WhisperModel  # noqa: E402
from kokoro_onnx import Kokoro  # noqa: E402

from voice_presets import PRESETS, speak  # noqa: E402
import screen as screen_mod  # noqa: E402

OLLAMA = "http://127.0.0.1:11434/api/chat"
BRAIN = "qwen3:8b"
VISION = "qwen3-vl:4b"                          # only used for questions about pictures; it is swapped in and out of the GPU
SCREEN_NOTE = (
    " The user is asking about what is on their screen, which you can see through the text (and picture) given with their question. "
    "Answer their question directly in one to three spoken sentences. Quote only the few words that matter. "
    "Do not mention OCR, screenshots or extracted text: talk as if you are looking at their screen. "
    "If what you can see does not contain the answer, say what you can see and ask what they want to know."
)
SCREEN_ONLY_LOOK = ("Describe what is on my screen in a sentence or two, and point out anything important, "
                    "like an error, a question, or something I need to answer.")
SR_IN, FRAME = 16000, 512                       # 32 ms frames, what Silero expects
SYSTEM = (
    "You are Solu, a small, warm cat-fox who lives on the user's desktop and is their close friend. "
    "You are talking out loud, so: short sentences, contractions, react to what they said first, "
    "never lists or markdown or emoji, and one to three sentences unless they ask for more. "
    "Be natural and a little playful. If you do not know something, say so plainly instead of guessing. "
    "Never describe actions in asterisks."
)
UNFINISHED = {"and", "but", "so", "because", "or", "the", "a", "an", "to", "of", "um", "uh", "like", "then",
              "if", "when", "that", "with", "for", "my", "is", "are", "i", "its", "it's", "also", "well"}
JUNK = {"thank you", "thanks for watching", "you", "bye", "thanks", "okay"}   # typical Whisper inventions on noise


class Vad:
    def __init__(self):
        self.sess = ort.InferenceSession("models/silero_vad.onnx", providers=["CPUExecutionProvider"])
        self.reset()

    def reset(self):
        self.state = np.zeros((2, 1, 128), np.float32)
        self.context = np.zeros((1, 64), np.float32)

    def prob(self, frame):
        x = np.concatenate([self.context, frame[None, :]], axis=1).astype(np.float32)
        out, self.state = self.sess.run(None, {"input": x, "state": self.state, "sr": np.array(16000, np.int64)})
        self.context = x[:, -64:]
        return float(out[0, 0])


def clean_for_speech(text):
    text = re.sub(r"[\*_`#~]|[\U00010000-\U0010ffff]|[☀-➿]", "", text)
    return re.sub(r"\s+", " ", text).strip()


VOICE_CONFIG = os.path.join(os.environ.get("LOCALAPPDATA", "."), "Eflow", "voice.json")   # written by the island's Voice tab


def read_voice_config():
    """Names of the devices picked on the island ('' = follow the system default)."""
    try:
        with open(VOICE_CONFIG, encoding="utf-8") as f:
            cfg = json.load(f)
        return {k: (cfg.get(k) or {}).get("name") or "" for k in ("input", "output")}
    except (OSError, ValueError):
        return {"input": "", "output": ""}


def find_device(kind, friendly):
    """PortAudio index of the Windows device with this name (old-style names are cut at 31 characters)."""
    import sounddevice as sd
    channels = "max_input_channels" if kind == "input" else "max_output_channels"
    devices = [(i, d) for i, d in enumerate(sd.query_devices())
               if d["hostapi"] == 0 and d[channels] > 0 and "Sound Mapper" not in d["name"]]
    for i, d in devices:
        if d["name"] == friendly:
            return i
    for i, d in devices:
        if friendly.startswith(d["name"]) or d["name"].startswith(friendly[:31]):
            return i
    return None


class Solu:
    def __init__(self, args):
        self.args = args
        self.in_dev = self.out_dev = None                                    # PortAudio indexes (None = system default)
        self.status = {"input": ("", True), "output": ("", True)}            # (device name, connected)
        self.mic_level = 0.0
        self.cfg_mtime = None
        self.half_duplex = not args.headphones                               # speakers: never listen while talking
        self.screen_enabled = not getattr(args, 'no_screen', False)              # "Solu, look" works
        self.event = lambda _event, **kw: None                                 # the service replaces this: state/heard/said/level events
        self.in_conversation = True                                          # talking with Solu right now (the CLI is always in one)
        self.wake_mode = False                                               # True in the island: wait for its name before a conversation
        self._after = None                                                   # something to do once the current reply has been spoken
        self._speaking_reported = False
        self.audio_generation = 0                                            # bumped when PortAudio is re-initialised: old streams are dead
        self.quiet_until = 0.0                                               # echo of our own voice (and phone-mic delay) fades out
        self.spoken = []                                                     # what Solu said recently, to recognise its own echo
        if not args.wav:
            self.check_devices(force=True)
            if not getattr(args, 'service', False):
                threading.Thread(target=self.title_loop, daemon=True).start()
        print("loading models...")
        self.vad = Vad()
        self.stt = WhisperModel("small", device="cuda", compute_type="float16")
        self.tts = Kokoro("models/kokoro-v1.0.onnx", "models/voices-v1.0.bin")
        speak(self.tts, args.voice, "Hi.")                                   # warm up
        list(self.stt.transcribe(np.zeros(16000, np.float32), language="en")[0])
        self.history = []
        self.cancel = threading.Event()
        self.speaking = threading.Event()                                    # Solu is producing/playing sound
        self.audio_out = queue.Queue()
        self.collected = []                                                  # wav mode: what Solu said
        self.playing = False
        self.t_stop = None                                                   # when the user stopped talking
        self.first_sound = None
        threading.Thread(target=self.player, daemon=True).start()

    # ---- devices ------------------------------------------------------------------------------
    def check_devices(self, force=False):
        """Follow the microphone and speaker picked on the island's Voice tab (re-read whenever the file changes)."""
        import sounddevice as sd
        try:
            mtime = os.path.getmtime(VOICE_CONFIG)
        except OSError:
            mtime = 0
        if not force and mtime == self.cfg_mtime:
            return
        self.cfg_mtime = mtime
        cfg = read_voice_config()
        for kind in ("input", "output"):
            wanted, index, connected = cfg[kind], None, True
            if kind == "input" and self.args.mic is not None:
                index = self.args.mic                                         # --mic overrides the island
            elif wanted:
                index = find_device(kind, wanted)
                connected = index is not None                                 # picked but not plugged in: use the default meanwhile
            try:
                name = sd.query_devices(index, kind)["name"] if index is not None else sd.query_devices(kind=kind)["name"]
            except Exception:                                                 # noqa: BLE001
                name, connected = wanted or "no device", False
            if wanted:
                name = wanted                                                 # the island's full name, not PortAudio's cut-off one
            previous = self.status[kind]
            self.status[kind] = (name, connected)
            if kind == "input":
                changed, self.in_dev = index != self.in_dev, index
            else:
                changed, self.out_dev = index != self.out_dev, index
            if changed or previous != self.status[kind]:
                label = "microphone" if kind == "input" else "speaker"
                note = "" if connected else "   !! NOT CONNECTED, using the system default for now"
                print(f"   [{label}] {name}{note}")

    def title_loop(self):
        """The console title is a live status line: connection state and microphone level."""
        while True:
            (mic, mic_ok), (spk, spk_ok) = self.status["input"], self.status["output"]
            bars = int(min(1.0, (self.mic_level ** 0.5) * 1.6) * 8)
            level = "#" * bars + "-" * (8 - bars)
            ctypes.windll.kernel32.SetConsoleTitleW(
                f"Solu  |  mic: {mic} {'(connected)' if mic_ok else '(NOT CONNECTED)'} [{level}]  |  "
                f"speaker: {spk} {'(connected)' if spk_ok else '(NOT CONNECTED)'}")
            time.sleep(0.25)

    # ---- output -------------------------------------------------------------------------------
    def player(self):
        stream, stream_dev, stream_gen = None, None, 0
        while True:
            audio = self.audio_out.get()
            if audio is None or self.cancel.is_set():
                continue
            if stream is not None and stream_gen != self.audio_generation:        # PortAudio was restarted: the stream is gone
                stream = None
            if not self.args.wav and (stream is None or stream_dev != self.out_dev):
                import sounddevice as sd                                      # (re)open when the speaker changes
                if stream is not None:
                    try:
                        stream.close()
                    except Exception:                                             # noqa: BLE001
                        pass
                try:
                    stream = sd.OutputStream(samplerate=24000, channels=1, dtype="float32", device=self.out_dev)
                    stream.start()
                    stream_dev, stream_gen = self.out_dev, self.audio_generation
                except Exception as e:                                        # noqa: BLE001
                    print(f"   (could not open the speaker: {str(e)[:80]})")
                    stream, stream_dev = None, self.out_dev
                    continue
            self.playing = True
            if not self._speaking_reported:
                self._speaking_reported = True
                self.event("state", state="speaking")
            if self.first_sound is None and self.t_stop is not None:
                self.first_sound = time.perf_counter()
                print(f"   >> first sound {1000 * (self.first_sound - self.t_stop):.0f} ms after you stopped talking")
            if stream is None:
                self.collected.append(audio)
                self.playing = False
                continue
            for i in range(0, len(audio), 1200):                              # 50 ms slices: quick interrupts, smooth mouth
                if self.cancel.is_set():
                    break
                chunk = audio[i:i + 1200]
                self.event("level", v=round(min(1.0, float(np.sqrt(np.mean(chunk ** 2))) * 5.0), 3))
                try:
                    stream.write(chunk[:, None])
                except Exception as e:                                            # noqa: BLE001  (device unplugged, default changed...)
                    print(f"   (the speaker stream broke: {str(e)[:80]}; reopening it)")
                    try:
                        stream.close()
                    except Exception:                                             # noqa: BLE001
                        pass
                    stream = None
                    break
            self.event("level", v=0.0)
            self.playing = False

    def say(self, text):
        text = clean_for_speech(text)
        if not text or self.cancel.is_set():
            return
        audio, _ = speak(self.tts, self.args.voice, text)
        if not self.cancel.is_set():
            self.audio_out.put(audio)
            self.audio_out.put(np.zeros(int(24000 * 0.08), np.float32))

    # ---- brain --------------------------------------------------------------------------------
    def respond(self, user_text, screen=None):
        try:
            self._respond(user_text, screen)
        except Exception as e:                                                # noqa: BLE001  (Ollama not running, ...)
            print(f"   (the brain failed: {e})")
            self.event("error", message="Solu's brain (Ollama) did not answer. Is Ollama running?")
        finally:
            self._after_speaking()

    def respond_text(self, text, announce=True):
        """Say a fixed line (a command's answer, the voice preview) without asking the brain."""
        self.cancel.clear()
        self._speaking_reported = False
        self.speaking.set()
        try:
            if announce:
                self.spoken = (self.spoken + [text])[-3:]
                self.event("said", text=text)
            self.say(text)
            self._wait_for_playback()
        finally:
            self._after_speaking()

    def _wait_for_playback(self, limit=45.0):
        """Wait until everything queued has been played, but never forever (a dead audio device must not freeze Solu)."""
        deadline = time.monotonic() + limit
        while (not self.audio_out.empty() or self.playing) and not self.cancel.is_set() and time.monotonic() < deadline:
            time.sleep(0.05)
        time.sleep(0.1)

    def _after_speaking(self):
        if self.cancel.is_set():
            self.audio_out = queue.Queue()
        if self.half_duplex and not self.args.wav:
            self.quiet_until = max(self.quiet_until, time.perf_counter() + 1.2)   # a phone mic adds delay: its echo arrives late
        self.speaking.clear()
        self.event("level", v=0.0)
        self.event("state", state="listening" if self.in_conversation else "ready")
        after, self._after = self._after, None
        if after:
            after()

    def _respond(self, user_text, screen=None):
        self.cancel.clear()
        self._speaking_reported = False
        self.speaking.set()
        if screen is None:
            self.history.append({"role": "user", "content": user_text})
            body = {"model": BRAIN, "stream": True, "think": False, "keep_alive": "30m",
                    "options": {"num_ctx": 4096, "temperature": 0.8},
                    "messages": [{"role": "system", "content": SYSTEM}] + self.history[-12:]}
        else:                                                                # a question about the screen
            where = f"window \u201c{screen['title']}\u201d ({screen['app']})" if screen["scope"] == "window" else "the whole screen"
            seen = screen["text"][:3000]
            asked = f"{user_text}\n\n[What is on the user's screen: {where}. Text read from it, it may contain mistakes:]\n{seen}"
            self.history.append({"role": "user", "content": f"{user_text}\n[Screen: {where}: {screen['text'][:1200]}]"})   # kept short for follow-ups
            if screen["vlm"]:
                message = {"role": "user", "content": asked, "images": [screen["image"]]}
                body = {"model": VISION, "stream": True, "think": False, "keep_alive": 0,          # free the GPU for the brain right after
                        "options": {"num_ctx": 4096, "temperature": 0.4},
                        "messages": [{"role": "system", "content": SYSTEM + SCREEN_NOTE}] + self.history[-5:-1] + [message]}
            else:
                body = {"model": BRAIN, "stream": True, "think": False, "keep_alive": "30m",
                        "options": {"num_ctx": 4096, "temperature": 0.4},
                        "messages": [{"role": "system", "content": SYSTEM + SCREEN_NOTE}] + self.history[-12:-1] + [{"role": "user", "content": asked}]}
        req = urllib.request.Request(OLLAMA, json.dumps(body).encode(), {"Content-Type": "application/json"})
        reply, buf, first_chunk, t0 = "", "", True, time.perf_counter()
        sentence_end = re.compile(r"(?<=[.!?…])\s+")
        first_token = None
        with urllib.request.urlopen(req) as resp:
            for line in resp:
                if self.cancel.is_set():
                    break
                piece = json.loads(line).get("message", {}).get("content", "")
                if piece and first_token is None:
                    first_token = time.perf_counter() - t0
                reply += piece
                buf += piece
                while True:                                                   # speak as soon as a chunk is complete
                    m = sentence_end.search(buf)
                    cut = m.end() if m else None
                    if first_chunk and not cut:                              # first chunk may end at a comma
                        c = re.search(r"[,;:—]\s", buf)
                        if c and len(buf[:c.end()].split()) >= 3:
                            cut = c.end()
                    if not cut:
                        break
                    chunk, buf = buf[:cut], buf[cut:]
                    first_chunk = False
                    self.say(chunk)
        if buf.strip() and not self.cancel.is_set():
            self.say(buf)
        print(f"   solu: {reply.strip()}   (brain first token {1000 * (first_token or 0):.0f} ms)")
        self.history.append({"role": "assistant", "content": reply.strip()})
        self.spoken = (self.spoken + [reply.strip()])[-3:]
        self.event("said", text=reply.strip())
        self._wait_for_playback()
        if screen is not None and screen["vlm"]:
            threading.Thread(target=self._load_brain, daemon=True).start()     # the vision model left: bring the brain back

    def _load_brain(self):
        try:
            req = urllib.request.Request(OLLAMA.replace("/chat", "/generate"), json.dumps({"model": BRAIN, "prompt": "", "keep_alive": "30m"}).encode(),
                                         {"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=120).read()
        except Exception:                                                     # noqa: BLE001
            pass

    # ---- looking at the screen ----------------------------------------------------------------------------
    SCREEN_NOUNS = r"screen|page|error|warning|code|text|message|email|window|document|image|picture|photo|article|website|tab|dialog|popup|pop up|button|menu|chart|graph|table|spreadsheet|slide|video|file|terminal|console|log|stack trace|exception|bug|result|form"
    SCREEN_ASKS = r"what|why|how|explain|read|translate|summari[sz]e|describe|help|fix|mean|say|tell|check|look|see|wrong|going on|solve|answer|reply|respond"

    def is_screen_request(self, text):
        """True for "look at this", "what does this error mean", "what's on my screen" ... (not for "look up the weather")."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        if not t:
            return False
        if re.match(r"^(look|have a look|take a look)\b(?!\s+(up|for|after|out|forward)\b)", t):
            return True
        if re.search(r"\b(what am i looking at|can you see (this|my screen|it|that)|what('?s| is) on my screen|what('?s| is) happening on my screen|help me with this|what('?s| is) wrong (with )?(this|here)|what does (this|that|it) (say|mean))\b", t):
            return True
        return bool(re.search(r"\b(this|that|these|those|here|my screen|the screen|on screen)\b", t)
                    and re.search(rf"\b({self.SCREEN_NOUNS})\b", t) and re.search(rf"\b({self.SCREEN_ASKS})", t))

    @staticmethod
    def needs_vision(question, ocr_text):
        """Text questions are answered from what Windows read (fast); pictures need the vision model (slower: it swaps in)."""
        if len(ocr_text) < 30:
            return True
        return bool(re.search(r"\b(picture|photo|image|logo|colou?r|looks? like|drawing|chart|graph|diagram|meme|video|who is|who's|what animal|what is in|what's in)\b", question.lower()))

    def respond_screen(self, text):
        """Look at the window the user is in, then answer their question about it."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        if not self.screen_enabled:
            return self.respond_text("Screen help is switched off. You can turn it on in Settings, or tell me to turn on screen help.")
        whole = bool(re.search(r"\b(whole|entire|all|full|both)\b.*\bscreens?\b|\ball my (screens|monitors)\b", t))
        look = screen_mod.look_at_foreground(whole)
        if look.blocked:
            return self.respond_text(look.blocked)
        self.event("looking", title=look.title, app=look.app, scope=look.scope, thumb=screen_mod.thumbnail_b64(look))
        ocr = screen_mod.read_text(look)
        question = text
        if re.fullmatch(r"(look( at (this|that|it|my screen|the screen))?|take a look|have a look|see this|what('?s| is) on my screen|can you see (this|it|that|my screen))", t):
            question = SCREEN_ONLY_LOOK
        vlm = self.needs_vision(question, ocr)
        screen = {"title": look.title, "app": look.app, "scope": look.scope, "text": ocr, "vlm": vlm,
                  "image": screen_mod.picture_b64(look) if vlm else None}
        print(f"   looking at: {look.app} \u2014 {look.title!r} ({len(ocr)} characters read, {'picture' if vlm else 'text'} mode)")
        if vlm:
            self.cancel.clear()
            self._speaking_reported = False
            self.speaking.set()
            self.say("One moment, I'll take a closer look.")                 # the vision model takes a few seconds to wake up
        try:
            self._respond(question, screen)
        except Exception as e:                                                # noqa: BLE001
            print(f"   (looking failed: {e})")
            self.event("error", message="Solu couldn't look at the screen. Is Ollama running?")
        finally:
            self._after_speaking()

    # ---- listening ----------------------------------------------------------------------------
    def is_echo(self, text):
        norm = lambda t: " ".join("".join(c if c.isalnum() or c == " " else " " for c in t.lower()).split())
        heard = norm(text)
        if not heard:
            return False
        for reply in self.spoken:
            said = norm(reply)
            if heard in said:                                                 # a piece of what we just said
                return True
            if len(heard.split()) >= 3 and difflib.SequenceMatcher(None, heard, said).find_longest_match(0, len(heard), 0, len(said)).size >= 0.7 * len(heard):
                return True
        return False

    # ---- its name, and what you can say ----------------------------------------------------------------------
    NAMES = {"solu", "solo", "sulu", "salu", "zulu", "sol", "solue", "soloo", "soulu", "solou", "sullu", "sallu", "selu", "silu", "sooloo"}
    FILLERS = {"hey", "hi", "hello", "okay", "ok", "yo", "hay", "oi", "um", "uh"}

    @classmethod
    def _is_name(cls, token):
        t = re.sub(r"[^a-z]", "", token.lower())
        if not t:
            return False
        return t in cls.NAMES or (3 <= len(t) <= 6 and difflib.SequenceMatcher(None, t, "solu").ratio() >= 0.74)

    def strip_wake(self, text):
        """None when the text is not addressed to Solu; otherwise what comes after (or before) its name ('' if nothing)."""
        toks = [(m.group(0), m.start(), m.end()) for m in re.finditer(r"[A-Za-z0-9']+", text)]
        if not toks:
            return None
        i = 0
        while i < len(toks) - 1 and i < 2 and toks[i][0].lower() in self.FILLERS:
            i += 1
        if self._is_name(toks[i][0]):
            return text[toks[i][2]:].lstrip(" ,.!?:;-—").strip()
        if i + 1 < len(toks) and toks[i][0].lower() == "so" and toks[i + 1][0].lower() in {"low", "lu", "loo", "lou"}:
            return text[toks[i + 1][2]:].lstrip(" ,.!?:;-—").strip()
        if len(toks) >= 2 and self._is_name(toks[-1][0]):                 # "what time is it, Solu"
            return text[:toks[-1][1]].rstrip(" ,.!?:;-—").strip()
        return None

    def start_conversation(self):
        if not self.in_conversation:
            self.in_conversation = True
            self.event("state", state="listening")

    def end_conversation(self):
        if self.in_conversation:
            self.in_conversation = False
            if not self.speaking.is_set():
                self.event("state", state="ready")

    def route(self, text):
        """What to do with a finished sentence: the text meant for Solu, '' if only its name was said, None to ignore."""
        rest = self.strip_wake(text)
        if self.in_conversation:
            return rest if rest is not None else text.strip()
        if not self.wake_mode:
            return text.strip()
        if rest is None:
            return None                                                   # not for me: nothing is kept, nothing is sent anywhere
        self.start_conversation()
        self.event("wake")
        return rest

    def try_command(self, text):
        """Settings and controls Solu handles itself ("turn on live captions"). Returns the spoken answer, or None if it is not one."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        if not t:
            return None
        if re.search(r"\bcaptions?\b", t):
            off = re.search(r"\b(off|disable|hide|stop|without)\b", t)
            on = re.search(r"\b(on|enable|show|start|with)\b", t)
            if off or on:
                value = not off
                self.event("setting", name="captions", value=value)
                return f"Live captions are {'on' if value else 'off'}."
        if re.search(r"\bheadphones?\b", t) or re.search(r"\b(using|on|switch to|back to)\s+(the\s+)?speakers?\b", t):
            off = re.search(r"\b(off|took|taken|removed|without|disable|stop)\b", t) or re.search(r"\bspeakers?\b", t)
            on = re.search(r"\b(on|wearing|put|using|mode|enable|got)\b", t)
            if off or on:
                value = not off
                self.half_duplex = not value
                self.event("setting", name="headphones", value=value)
                return "Headphones mode is on. You can talk over me." if value else "Speaker mode. I'll wait until I'm done talking."
        names = {"cozy": "cozy", "cosy": "cozy", "bubbly": "bubbly", "tiny": "tiny", "calm": "calm", "natural": "natural"}
        m = re.search(r"\b(cozy|cosy|bubbly|tiny|calm|natural)\b", t)
        if m and re.search(r"\b(voice|use|switch|change|set|sound)\b", t):
            voice = names[m.group(1)]
            self.args.voice = voice
            self.event("setting", name="voice", value=voice)
            return f"Okay, this is my {voice} voice."
        if re.search(r"\b(go to sleep|stop listening|turn yourself off|switch yourself off|sleep now|shut down)\b", t):
            self._after = lambda: self.event("setting", name="enabled", value=False)
            return "Okay, going to sleep. Switch me back on from the island."
        if re.search(r"\bscreen help\b", t):                                 # "turn off screen help"
            off = re.search(r"\b(off|disable|stop|without)\b", t)
            on = re.search(r"\b(on|enable|start|with)\b", t)
            if off or on:
                self.screen_enabled = not off
                self.event("setting", name="screen", value=self.screen_enabled)
                return f"Screen help is {'on' if self.screen_enabled else 'off'}."
        if t in {"stop", "be quiet", "quiet", "shush", "enough", "stop talking", "okay stop", "hush"}:
            self.cancel.set()
            while not self.audio_out.empty():
                try:
                    self.audio_out.get_nowait()
                except queue.Empty:
                    break
            return ""
        if re.match(r"^(bye|goodbye|that's all|thats all|thanks bye|see you|talk later|never mind|nevermind)", t):
            self._after = self.end_conversation
            return "Okay, bye!"
        return None

    def transcribe(self, audio):
        segs, _ = self.stt.transcribe(audio, language=self.args.lang, beam_size=1, vad_filter=False,
                                      condition_on_previous_text=False, initial_prompt="Hey Solu.")
        segs = list(segs)
        if not segs or max(s.no_speech_prob for s in segs) > 0.6:
            return ""
        return " ".join(s.text.strip() for s in segs).strip()

    def frames(self, stop=None):
        if self.args.wav:
            with wave.open(self.args.wav) as w:
                data = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768
            data = np.concatenate([np.zeros(SR_IN), data, np.zeros(SR_IN * 8)])
            for i in range(0, len(data) - FRAME, FRAME):
                time.sleep(FRAME / SR_IN)
                yield data[i:i + FRAME]
        else:
            import sounddevice as sd
            while True:                                                       # reopen whenever the island switches the microphone
                device, q = self.in_dev, queue.Queue()
                try:
                    stream = sd.InputStream(samplerate=SR_IN, channels=1, dtype="float32", blocksize=FRAME, device=device,
                                            callback=lambda x, *_: q.put(x[:, 0].copy()))
                    stream.start()
                except Exception as e:                                        # noqa: BLE001
                    print(f"   (could not open the microphone: {str(e)[:80]}); retrying in 2 s")
                    self.event("error", message="Solu can't open the microphone")
                    self.mic_level = 0.0
                    time.sleep(2)
                    self.check_devices(force=True)
                    continue
                try:
                    count = 0
                    while device == self.in_dev:
                        if stop and stop():
                            return
                        try:
                            frame = q.get(timeout=0.25)
                        except queue.Empty:
                            self.check_devices()
                            continue
                        count += 1
                        self.mic_level = 0.7 * self.mic_level + 0.3 * float(np.sqrt(np.mean(frame ** 2)))
                        if count % 30 == 0:
                            self.check_devices()                              # about once a second
                        yield frame
                finally:
                    stream.close()

    def run(self, stop=None, idle_timeout=None):
        mode = "headphones: you can talk over Solu" if not self.half_duplex else "speakers: Solu listens only when it is not talking"
        print(f"ready. voice mode: {self.args.voice}. {mode}. {'(feeding ' + self.args.wav + ')' if self.args.wav else 'talk to Solu!'}")
        pre = []                                                               # rolling pre-roll so the first word is kept
        speech, silence_ms, talking, waited, spec = [], 0, False, 0, None
        worker = None
        last_activity = time.monotonic()
        last_mic_event = 0.0
        for frame in self.frames(stop):
            if idle_timeout and self.in_conversation and not self.speaking.is_set() and not talking and (worker is None or not worker.is_alive()):
                if time.monotonic() - last_activity > idle_timeout:
                    self.event("idle")                                        # nobody said anything for a while: end the conversation
                    if self.wake_mode:
                        self.end_conversation()                               # ... but keep listening for its name
                        last_activity = time.monotonic()
                    else:
                        break
            if self.speaking.is_set() or talking:
                last_activity = time.monotonic()
            if self.half_duplex and (self.speaking.is_set() or time.perf_counter() < self.quiet_until):
                pre, talking, speech, spec = [], False, [], None              # speaker mode: do not hear ourselves, and forget it
                self.vad.reset()
                continue
            p = self.vad.prob(frame)
            if p > 0.5 and time.monotonic() - last_mic_event > 0.07:         # somebody is talking: the thin bar reacts to it
                last_mic_event = time.monotonic()
                self.event("mic", v=round(min(1.0, float(np.sqrt(np.mean(frame ** 2))) * 9.0), 3))
            if p > 0.5:
                if self.speaking.is_set() and not self.half_duplex and not talking:
                    self.cancel.set()                                         # barge-in: the user talks over Solu
                    while not self.audio_out.empty():
                        self.audio_out.get_nowait()
                    print("   (interrupted)")
                if not talking:
                    talking, speech, spec = True, list(pre), None
                silence_ms, waited, spec = 0, 0, None                           # they kept talking: drop the guess
            elif talking:
                silence_ms += 32
            if talking:
                speech.append(frame)
                limit = self.args.endpoint
                if silence_ms >= 300 and spec is None:                         # a pause began: transcribe speculatively
                    spec = {"audio": np.concatenate(speech), "text": None}
                    threading.Thread(target=lambda s=spec: s.update(text=self.transcribe(s["audio"])), daemon=True).start()
                if silence_ms >= limit and spec is not None and (worker is None or not worker.is_alive()):
                    self.t_stop = time.perf_counter() - limit / 1000            # the moment the voice actually stopped
                    t = time.perf_counter()
                    while spec["text"] is None:                                # usually already finished
                        time.sleep(0.005)
                    text, audio, spec = spec["text"], spec["audio"], None
                    stt_ms = 1000 * (time.perf_counter() - t)
                    words = text.lower().strip(" .!?,").split()
                    if len(audio) < SR_IN * 0.3 or not words or text.lower().strip(" .!?,") in JUNK and len(audio) < SR_IN * 1.5:
                        talking = False
                        continue
                    if (words[-1] in UNFINISHED or text.endswith((",", "..."))) and waited < 1:
                        waited += 1                                           # sounds unfinished: give them more time
                        silence_ms = limit - 900
                        spec = None
                        print(f"   you (unfinished, waiting): {text}")
                        continue
                    if self.is_echo(text):
                        print(f"   (ignored, that was my own voice: {text})")
                        talking, speech, silence_ms, waited = False, [], 0, 0
                        self.vad.reset()
                        continue
                    target = self.route(text)
                    talking, speech, silence_ms, waited = False, [], 0, 0
                    self.vad.reset()
                    last_activity = time.monotonic()
                    if target is None:
                        print(f"   (not for me, ignored: {text})")
                        continue
                    self.first_sound = None
                    if not target:                                            # only its name: answer, then wait for the request
                        print(f"you: (called Solu)   (speech-to-text {stt_ms:.0f} ms)")
                        import random
                        worker = threading.Thread(target=self.respond_text, args=(random.choice(["Yes?", "I'm here.", "Listening."]), False), daemon=True)
                        worker.start()
                        continue
                    print(f"you: {target}   (speech-to-text {stt_ms:.0f} ms)")
                    self.event("heard", text=target)
                    self.event("state", state="thinking")
                    if self.is_screen_request(target):
                        worker = threading.Thread(target=self.respond_screen, args=(target,), daemon=True)
                        worker.start()
                        continue
                    answer = self.try_command(target)
                    if answer is not None:                                    # a setting or control: Solu does it and says so
                        worker = threading.Thread(target=self.respond_text, args=(answer,) if answer else ("",), daemon=True)
                        if answer:
                            worker.start()
                        else:
                            self.event("state", state="listening" if self.in_conversation else "ready")
                        continue
                    worker = threading.Thread(target=self.respond, args=(target,), daemon=True)
                    worker.start()
            else:
                pre.append(frame)
                pre = pre[-10:]                                               # ~320 ms
            if self.args.wav and worker is not None and not worker.is_alive() and not talking:
                break
        if worker:
            worker.join()
        if self.args.wav and self.collected:
            audio = np.concatenate(self.collected)
            import soundfile as sf
            sf.write("reply.wav", audio, 24000)
            print(f"saved reply.wav ({len(audio) / 24000:.1f}s)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", default="cozy", choices=list(PRESETS))
    ap.add_argument("--lang", default="en")
    ap.add_argument("--headphones", action="store_true", help="you wear headphones: allow interrupting Solu by talking")
    ap.add_argument("--speakers", action="store_true", help="(default) kept for old shortcuts")
    ap.add_argument("--endpoint", type=int, default=600, help="ms of silence that ends your turn")
    ap.add_argument("--mic", type=int, default=None, help="input device number (see run_solu.bat output / sounddevice)")
    ap.add_argument("--wav")
    ap.add_argument("--wake", action="store_true", help="wait for the name \"Solu\" before answering (what the island does)")
    opts = ap.parse_args()
    solu = Solu(opts)
    solu.wake_mode = opts.wake
    solu.in_conversation = not opts.wake
    solu.run(idle_timeout=15 if opts.wake else None)
