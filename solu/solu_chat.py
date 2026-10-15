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
import actions
import gpu  # noqa: E402
import memory as memory_mod  # noqa: E402
import research as research_mod  # noqa: E402
import tasks as tasks_mod  # noqa: E402
import skills  # noqa: E402

OLLAMA = "http://127.0.0.1:11434/api/chat"
BRAIN = "qwen3:8b"
CTX = 8192                                      # one context size for every brain call: a change makes Ollama reload the model
VISION = "qwen3-vl:4b"                          # only used for questions about pictures; it is swapped in and out of the GPU
SCREEN_NOTE = (
    " The user is asking about what is on their screen, which you can see through the text (and picture) given with their question. "
    "Answer their question directly in one to three spoken sentences. Quote only the few words that matter. "
    "Do not mention OCR, screenshots or extracted text: talk as if you are looking at their screen. "
    "If what you can see does not contain the answer, say what you can see and ask what they want to know."
)
TRANSLATE_NOTE = (
    " The user wants a translation of what is on their screen. Translate the visible text {target}. Give only the translation, "
    "complete and faithful, keeping names and numbers; skip menus, buttons and other interface text. No comments before or after."
)
CODE_NOTE = (
    " The user is stuck on an error or a problem in code on their screen. In two to four spoken sentences: say what the error means, "
    "the most likely cause, and the concrete fix (the exact change or command). If the screen doesn't show enough, say what you need."
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
AGENT_NOTE = (
    " You can also use the user's Windows PC for them with your tools: open apps and websites, search the web, control music and volume, "
    "set timers, read and click things on the screen, type, press shortcuts, manage windows, find, open, read and write files, "
    "use the clipboard and run PowerShell commands. When they ask you to do something on the PC, use a tool instead of explaining how; "
    "for plain chat, just talk. Every action that needs it is shown to the user on an approval card, which they answer, so you don't "
    "need to ask in words. Prefer the simplest tool (open_app for apps, web_search to search, media_control for music) and use "
    "run_command only when nothing else fits. If a step needs what is on the screen, use look_at_screen first. Check each tool result. "
    "Finish the whole task: to ask a website something (for example ChatGPT), open_website, then type_text the message, then press_keys "
    "'enter'; to find something, use find_files with the user's own words. For a bigger task, such as researching a topic and then "
    "emailing someone about it, work through the steps in order: use research to learn about the topic, write the email, then call "
    "send_email, which shows the user the full email on an approval card so they decide whether it is sent. If the recipient's address "
    "is missing or unclear (speech often garbles addresses), don't refuse: still research and write it, and call send_email with an "
    "empty 'to'; it then opens as a Gmail draft where the user adds the address. "
    "Answer only what the user just said: don't bring up old topics or look at the screen unless they ask. "
    "Don't create or change files unless the user asked for a file. "
    "For a goal that needs many separate actions over minutes (about four or more steps, or 'in the background', 'keep working', "
    "'by yourself'), call start_task with the goal in the user's words instead of doing it step by step here; the plan is shown for approval. "
    "For a quick question about a topic, use research and answer out loud. When they want deep or in-depth research, a report or a "
    "document, call deep_research once: it works in the background and opens the report in Chrome, so just tell them you're on it. "
    "Don't talk while calling tools; when everything is done, say in one short sentence what you did, or what went wrong. "
    "Only report facts that a tool result actually contains: never invent numbers, names or results, and never claim something "
    "worked if the tool said error or returned nothing; then try another way or say you couldn't. "
    f"The user's home folder is {actions.HOME}."
)
YES = re.compile(r"\b(yes|yeah|yep|yup|sure|ok|okay|go ahead|do it|allow|approve|approved|please do|of course|fine|go on|alright)\b")
NO = re.compile(r"\b(no|nope|don't|dont|do not|cancel|stop|never ?mind|wait|not now|deny|reject)\b")
EMOJI = re.compile(r"\s*[\U0001F000-\U0001FAFF☀-➿️]+")
ALL = re.compile(r"\b(yes to (all|everything)|(do|allow) (it )?all|all of (it|them)|everything|the whole thing|all (the )?steps|go ahead with (it )?all)\b")
PROMISE = re.compile(r"\b(let me(?! know)|i'll(?! be)|i will|i'm going to|i am going to|checking|one moment|one sec|give me a (sec|second)|next,? i)\b")
ALWAYS = re.compile(r"\b(always|every time|don't ask( me)? again|dont ask)\b")
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


def live_default_input():
    """Name of the microphone Windows uses as its default RIGHT NOW (cut to 31 characters, like PortAudio's), or None.
    PortAudio decides its own default once, when it starts, and never updates it: a mic that was plugged in or made the
    default afterwards (or a phone-webcam mic that grabbed the default earlier) would be heard instead. Windows' mapper
    device is asked live, so Solu always agrees with the island."""
    try:
        winmm = ctypes.WinDLL("winmm")

        class Caps(ctypes.Structure):
            _fields_ = [("m", ctypes.c_ushort), ("p", ctypes.c_ushort), ("v", ctypes.c_uint), ("name", ctypes.c_wchar * 32),
                        ("f", ctypes.c_uint), ("ch", ctypes.c_ushort), ("r", ctypes.c_ushort)]

        dev, flags = ctypes.c_uint(), ctypes.c_uint()
        if winmm.waveInMessage(ctypes.c_void_p(0xFFFFFFFF), 0x2015, ctypes.byref(dev), ctypes.byref(flags)) != 0 or dev.value == 0xFFFFFFFF:
            return None
        caps = Caps()
        if winmm.waveInGetDevCapsW(dev.value, ctypes.byref(caps), ctypes.sizeof(caps)) != 0:
            return None
        return caps.name or None
    except Exception:                                                         # noqa: BLE001
        return None


def mapper_index(kind):
    """PortAudio index of Windows' sound mapper: opening it picks whatever the Windows default is at that moment."""
    import sounddevice as sd
    channels = "max_input_channels" if kind == "input" else "max_output_channels"
    for i, d in enumerate(sd.query_devices()):
        if d["hostapi"] == 0 and d[channels] > 0 and "Sound Mapper" in d["name"]:
            return i
    return None


class Solu:
    def __init__(self, args):
        self.args = args
        self.in_dev = self.out_dev = None                                    # PortAudio indexes (None = system default)
        self.live_default = None                                             # the default microphone Windows reported last
        self.in_epoch = 0                                                    # bumped whenever the microphone must be reopened
        self.need_pa_refresh = False                                         # PortAudio's device list is out of date (a device was plugged in)
        self._pa_refreshed_for = None
        self.status = {"input": ("", True), "output": ("", True)}            # (device name, connected)
        self.mic_level = 0.0
        self.cfg_mtime = None
        self.half_duplex = not args.headphones                               # speakers: never listen while talking
        self.screen_enabled = not getattr(args, 'no_screen', False)              # "Solu, look" works
        self.hands = actions.Hands(self)                                     # what Solu can do on the PC
        self.memory = memory_mod.Memory()                                    # lasting facts about the user
        self.conversation_from = 0                                           # where the current conversation starts in the history
        self.jobs = []                                                       # research running in the background
        self.chat_priority_until = 0.0                                       # the user is talking: background work keeps out of the model's way
        self.tasks = None                                                    # long tasks (the task runner starts with the service)
        self.clipboard = skills.ClipboardHistory()                           # what was copied lately (memory only, never on disk)
        self.routines = skills.Routines()                                    # "when I say study mode, ..."
        self.quiz = None                                                     # the study buddy, while a quiz runs
        self.notes = None                                                    # lecture / meeting notes, while recording
        self.context = {}                                                    # what Eflow shares: notification senders and counts
        self.paused = False                                                  # privacy key: the microphone is closed
        self.stt_lock = threading.Lock()                                     # Whisper is shared by the mic and the note taker
        if getattr(args, "service", False):
            self.tasks = tasks_mod.TaskRunner(self, OLLAMA, BRAIN, CTX)          # long goals, done in the background
            skills.Nudges(self).start()                                      # rare, quiet pills (battery, tomorrow's plans, breaks)
        self.approvals = {}                                                  # approval id -> {"event", "answer"}
        self.pending_approval = None                                         # an approval card is waiting for the user
        self._approval_seq = 0
        self._told_about_card = False
        actions.preload()                                                    # the list of installed apps, in the background
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
        on_cpu = os.environ.get("SOLU_STT_CPU") == "1"                         # tests: a second copy must not take more of the graphics card
        self.stt = WhisperModel("small", device="cpu" if on_cpu else "cuda", compute_type="int8" if on_cpu else "float16")
        self.tts = Kokoro("models/kokoro-v1.0.onnx", "models/voices-v1.0.bin")
        speak(self.tts, args.voice, "Hi.")                                   # warm up
        list(self.stt.transcribe(np.zeros(16000, np.float32), language="en")[0])
        threading.Thread(target=self._load_brain, daemon=True).start()     # the brain is ready before the first question
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
        live = live_default_input()
        if live != self.live_default:                                         # Windows' default microphone changed: follow it
            self.live_default = live
            self.in_epoch += 1
            force = True
        try:
            mtime = os.path.getmtime(VOICE_CONFIG)
        except OSError:
            mtime = 0
        if not force and mtime == self.cfg_mtime:
            return
        self.cfg_mtime = mtime
        cfg = read_voice_config()
        for kind in ("input", "output"):
            wanted, index, connected, following = cfg[kind], None, True, False
            if kind == "input" and self.args.mic is not None:
                index = self.args.mic                                         # --mic overrides the island
            elif wanted:
                index = find_device(kind, wanted)
                connected = index is not None                                 # picked but not plugged in: use the default meanwhile
                if index is None and kind == "input":
                    if self._pa_refreshed_for != wanted:                      # plugged in after Solu started: PortAudio's list is stale
                        self._pa_refreshed_for = wanted
                        self.need_pa_refresh = True
                        self.in_epoch += 1
                    index, following = mapper_index("input"), True            # meanwhile: the live Windows default, not a frozen one
            elif kind == "input":
                index, following = mapper_index("input"), True                # no pick: follow Windows' default, live
            try:
                name = sd.query_devices(index, kind)["name"] if index is not None else sd.query_devices(kind=kind)["name"]
            except Exception:                                                 # noqa: BLE001
                name, connected = wanted or "no device", False
            if following and live and not wanted:
                name = live                                                   # what Windows really uses now, not "Sound Mapper"
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
            parts = re.split(r"(?<=[.!?])\s+", text) if len(text) > 220 else [text]   # a long reading starts at once, sentence by sentence
            for part in parts:
                if self.cancel.is_set():
                    break
                self.say(part)
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
            return self._agent(user_text)
        else:                                                                # a question about the screen
            where = f"window \u201c{screen['title']}\u201d ({screen['app']})" if screen["scope"] == "window" else "the whole screen"
            seen = screen["text"][:3000]
            asked = f"{user_text}\n\n[What is on the user's screen: {where}. Text read from it, it may contain mistakes:]\n{seen}"
            before = self.recent(14, plain=True)                              # the conversation so far, words only
            self.history.append({"role": "user", "content": f"{user_text}\n[Screen: {where}: {screen['text'][:1200]}]"})   # kept short for follow-ups
            if screen["vlm"]:
                message = {"role": "user", "content": asked, "images": [screen["image"]]}
                body = gpu.body(VISION, {"num_ctx": 4096, "temperature": 0.4}, keep=0, stream=True, think=False,   # free the GPU right after
                                messages=[{"role": "system", "content": SYSTEM + screen.get("note", SCREEN_NOTE)}] + before[-4:] + [message])
            else:
                body = gpu.body(BRAIN, {"num_ctx": CTX, "temperature": 0.4}, stream=True, think=False,
                                messages=[{"role": "system", "content": SYSTEM + screen.get("note", SCREEN_NOTE)}] + before + [{"role": "user", "content": asked}])
        reply, _, first_token = self._stream(body)
        print(f"   solu: {reply.strip()}   (brain first token {1000 * (first_token or 0):.0f} ms)")
        self.history.append({"role": "assistant", "content": reply.strip()})
        self.spoken = (self.spoken + [reply.strip()])[-3:]
        self.event("said", text=reply.strip())
        self._wait_for_playback()
        if screen["vlm"]:
            threading.Thread(target=self._load_brain, daemon=True).start()     # the vision model left: bring the brain back

    def _stream(self, body):
        """Ask the model, speaking its words as they arrive. Returns (text, tool calls, seconds to the first token)."""
        req = urllib.request.Request(OLLAMA, json.dumps(body).encode(), {"Content-Type": "application/json"})
        reply, buf, first_chunk, t0, calls, mute = "", "", True, time.perf_counter(), [], False
        sentence_end = re.compile(r"(?<=[.!?…])\s+")
        first_token = None
        with urllib.request.urlopen(req) as resp:
            for line in resp:
                if self.cancel.is_set():
                    break
                message = json.loads(line).get("message", {})
                calls += message.get("tool_calls") or []
                piece = message.get("content", "")
                if piece and first_token is None:
                    first_token = time.perf_counter() - t0
                reply += piece
                buf += piece
                if mute or "<tool" in buf or "<|" in buf:                    # the model wrote a tool call as text: never say it
                    if not mute:
                        before = buf[:buf.find("<")]
                        if before.strip():
                            self.say(before)
                    mute, buf = True, ""
                    continue
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
        if buf.strip() and not self.cancel.is_set() and not mute:
            self.say(buf)
        return reply, calls, first_token

    @staticmethod
    def text_tool_calls(reply):
        """Pull '<tool_call>{json}</tool_call>' (written as text instead of a real call) out of a reply: (clean text, calls)."""
        calls = []
        for block in re.findall(r"<tool_call>\s*(\{.*?\})\s*(?:</tool_call>|$)", reply, re.S):
            try:
                data = json.loads(block)
                calls.append({"function": {"name": data.get("name", ""), "arguments": data.get("arguments") or {}}})
            except ValueError:
                pass
        clean = re.sub(r"<tool_call>.*?(</tool_call>|$)|<\|[^>]*\|>", "", reply, flags=re.S).strip()
        return clean, calls

    def recent(self, n, plain=False):
        """The last messages of the conversation, starting at something the user said. plain: only the words (no tool calls)."""
        msgs = self.history[-n:]
        while msgs and msgs[0]["role"] != "user":
            msgs = msgs[1:]
        if plain:
            msgs = [{"role": m["role"], "content": m["content"]} for m in msgs if m["role"] in ("user", "assistant") and m["content"].strip()]
        return msgs

    # ---- doing things on the PC ------------------------------------------------------------------------------
    def _agent(self, user_text):
        """Talk, or act: the brain may call tools (approved by the user where needed), see the results and carry on."""
        turn = [{"role": "user", "content": user_text}]                       # this request, with its real tool calls and results
        self.request_text = user_text
        said, did, t0, nudges, done, repeats = [], [], time.perf_counter(), 0, {}, 0
        self.allow_all = getattr(self, "allow_all_next", False)              # "yes to all" (or a routine): no cards for safe steps
        self.allow_all_next = False
        follow_up = len(user_text.split()) <= 8 and re.search(r"\b(it|that|this|them|again|do it|send|go ahead|now|too|also)\b", user_text.lower())
        tools = actions.pick_tools(user_text, getattr(self, "last_tools", ()) if follow_up else ())
        self.last_tools = [t["function"]["name"] for t in tools]
        name = re.search(r"\b(?:my name is|call me)\s+([A-Za-z][\w'-]{1,30})", user_text, re.I)
        if name:                                                              # the one fact worth saving the moment it is said
            self.memory.add(f"The user's name is {name.group(1).capitalize()}.", "name")
        facts = self.memory.relevant(user_text)                               # only what relates to this request (+ name, preferences)
        known = ("\nWhat you remember about the user (use it naturally when it helps; don't recite it unprompted):\n" +
                 "\n".join(f"- {f}" for f in facts)) if facts else ""
        math_result = actions.spoken_math(user_text)
        if math_result:                                                       # sums are computed, never guessed
            known += f"\nExact calculation (use this result): {math_result[0]} = {math_result[1]}"
        if any(t["function"]["name"] == "system_info" for t in tools):        # time, date, battery, disks: fetched, never guessed
            known += f"\nRight now (real, use these exact values): {self.hands.run('system_info', {})}"
            tools = [t for t in tools if t["function"]["name"] != "system_info"]
        forced = False                                                        # it said "one moment" over and over: made to answer once
        for _ in range(14):                                                   # enough steps for a longer task
            if repeats >= 2:
                break
            past = self.recent(14) if tools else self.recent(14, plain=True)   # no tools: no old tool calls in sight to imitate
            messages = [{"role": "system", "content": SYSTEM + (AGENT_NOTE if tools else "") + known}] + past + turn
            body = gpu.body(BRAIN, {"num_ctx": CTX, "temperature": 0.5}, stream=True, think=False, messages=messages)
            if tools:
                body["tools"] = tools                                         # only what this request asks for; plain chat gets none
            reply, calls, first_token = self._stream(body)
            if "<tool" in reply or "<|" in reply:                             # a call written as text: keep it only if this request may use it
                reply, written = self.text_tool_calls(reply)
                allowed = {t["function"]["name"] for t in tools}
                calls = calls + [c for c in written if c["function"]["name"] in allowed]
            if not re.search(r"\w", reply):                                   # a lone "?" or "." between tool calls
                reply = ""
            if reply.strip():
                said.append(reply.strip())
                print(f"   solu: {reply.strip()}   (brain first token {1000 * (first_token or 0):.0f} ms)")
            if self.cancel.is_set():
                break
            if not calls:
                # "I'll check that for you..." and then nothing: words don't do anything, so push it to act once.
                if nudges < 3 and not forced and PROMISE.search(reply.lower()):
                    nudges += 1
                    turn += [{"role": "assistant", "content": reply},
                             {"role": "user", "content": "(Eflow: nothing has happened yet. Call the tool now, without commenting on this note.)"}]
                    continue
                if reply.strip():
                    turn.append({"role": "assistant", "content": reply.strip()})
                break
            looks = all(c.get("function", {}).get("name") in ("look_at_screen", "system_info") for c in calls)
            if reply.strip() and not PROMISE.search(reply.lower()) and (did or looks):
                # It has answered. A look it was not asked for (or one more round after acting) would only make it talk twice.
                turn.append({"role": "assistant", "content": reply.strip()})   # it already acted and has answered: extra calls are noise
                break
            turn.append({"role": "assistant", "content": reply, "tool_calls": calls})
            fresh, refused = 0, False
            for call in calls:
                name = call.get("function", {}).get("name", "")
                args = call.get("function", {}).get("arguments") or {}
                if isinstance(args, str):
                    try:
                        args = json.loads(args)
                    except ValueError:
                        args = {}
                key = name + json.dumps(args, sort_keys=True)
                if key in done:                                               # small models like to repeat a call that already worked
                    repeats += 1
                    result = f"already done a moment ago ({done[key][:200]}). Do not do it again; just answer the user."
                else:
                    result = done[key] = self.use_tool(name, args)
                    did.append(name)
                    fresh += 1
                print(f"   [tool] {name}({json.dumps(args, ensure_ascii=False)[:200]}) -> {result[:200]!r}")
                turn.append({"role": "tool", "content": result[:2000], "tool_name": name})
                if result.startswith(("the user said no", "the user did not answer")):
                    refused = True
                    break
                if self.cancel.is_set():
                    break
            if refused:                                                       # "no" ends the request: nothing else is tried
                text = "Okay, I won't." if result.startswith("the user said no") else "Okay, I'll leave it for now."
                said.append(text)
                self.say(text)
                turn.append({"role": "assistant", "content": text})
                break
            if not fresh and reply.strip():                                   # it answered and only repeated itself: the answer stands
                if PROMISE.search(reply.lower()) and not forced:              # ... unless that "answer" is only "one moment, I'm looking": make it answer now
                    forced, tools, repeats = True, [], 0
                    turn.append({"role": "user", "content": "(Eflow: you already have what the tool showed. Answer the user now in one or two plain "
                                                            "sentences. Do not call a tool and do not say you are still looking.)"})
                    continue
                break
        text = re.sub(r"(\s+[?.!,]+)+$", "", " ".join(said).strip())
        if said and said[-1] in ("Okay, I won't.", "Okay, I'll leave it for now.") and len(said) > 1:
            text = said[-1]                                                   # don't show half-plans for something that was refused
        if not text and did:                                                  # it acted but said nothing: still answer
            text = "Done."
            self.say(text)
        print(f"   (request handled in {time.perf_counter() - t0:.1f} s, {len(did)} action(s))")
        for m in turn:                                                        # remember it, but keep old tool output short
            if m["content"].startswith("(Eflow:"):
                continue
            self.history.append(dict(m, content=m["content"][:300]) if m["role"] == "tool" else m)
        text = EMOJI.sub("", text).strip()
        self.spoken = (self.spoken + [text])[-3:]
        self.event("said", text=text)
        self._wait_for_playback()

    def use_tool(self, name, args):
        if name not in actions.LEVEL:
            return f"error: there is no tool called {name}"
        if name == "write_file" and not re.search(r"\b(file|save|note|notes|document|doc|txt|write (it|that|this) down|folder)\b",
                                                   getattr(self, "request_text", "").lower()):
            return "skipped: the user did not ask for a file. Don't make files; carry on with what they asked."   # small models love extra files
        if actions.needs_approval(name, args) and not (getattr(self, "allow_all", False) and not actions.describe(name, args)[2]):
            answer = self.ask_permission(name, args)
            if answer == "deny":
                return "the user said no. Do not do it and do not try another way; just say okay."
            if answer == "timeout":
                return "the user did not answer, so it was not done."
            if answer == "always":
                actions.trust(name)
            if answer == "all":
                self.allow_all = True                                         # the remaining safe steps of this request go ahead
        self.event("action", tool=name)
        return self.hands.run(name, args)

    def ask_permission(self, name, args, background=False):
        """Show the approval card and wait for a tap (or a spoken yes / no). Returns allow, always, deny or timeout.
        background: asked by a task, not in the middle of a conversation turn: it waits for the conversation, leaves its flags alone."""
        action, detail, risky, can_always = actions.describe(name, args)
        if background:
            return self._ask_permission_background(name, action, detail, risky, can_always)
        self._wait_for_playback()                                             # let Solu finish its sentence first
        self._approval_seq += 1
        aid = self._approval_seq
        waiter = threading.Event()
        self.approvals[aid] = {"event": waiter, "answer": None}
        self.event("approve", id=aid, action=action, detail=detail, risky=risky, always=can_always)
        question = f"Can I {action}?" if self._told_about_card else f"Can I {action}? Tap Approve on the island, or just say yes."
        self._told_about_card = True
        self.say(question)
        self._wait_for_playback()
        print(f"   [approval {aid}] {action}: {detail[:120]}{'  (RISKY)' if risky else ''} -- waiting for the user")
        self.pending_approval = aid
        if self.half_duplex:
            self.quiet_until = max(self.quiet_until, time.perf_counter() + 0.8)
        self.speaking.clear()                                                 # the microphone listens for "yes" / "no"
        self.event("state", state="listening")
        deadline = time.monotonic() + 120
        while not waiter.wait(0.2) and not self.cancel.is_set() and time.monotonic() < deadline:
            pass
        self.pending_approval = None
        answer = self.approvals.pop(aid, {}).get("answer") or ("deny" if self.cancel.is_set() else "timeout")
        self.event("resolved", id=aid)
        self._speaking_reported = False
        self.speaking.set()
        self.event("state", state="thinking")
        print(f"   [approval {aid}] {answer}")
        return answer

    def _ask_permission_background(self, name, action, detail, risky, can_always):
        t0 = time.monotonic()
        while self.chat_busy() and time.monotonic() - t0 < 120:               # not on top of what is being said
            time.sleep(0.3)
        self._approval_seq += 1
        aid = self._approval_seq
        waiter = threading.Event()
        self.approvals[aid] = {"event": waiter, "answer": None}
        self.event("approve", id=aid, action=action, detail=detail, risky=risky, always=can_always)
        what = "run this task" if name == "run_task" else action
        question = f"Can I {what}? Tap Approve on the island, or just say yes." if not self._told_about_card else f"Can I {what}?"
        self._told_about_card = True
        print(f"   [approval {aid}] {action}: {detail[:100]}{'  (RISKY)' if risky else ''} -- waiting for the user (task)")
        self.pending_approval = aid
        self.start_conversation()                                             # so a plain "yes" is heard without the name
        self.respond_text(question)
        deadline = time.monotonic() + 15 * 60
        while not waiter.wait(0.3) and time.monotonic() < deadline:
            if self.tasks and self.tasks.cancelled():
                break
        self.pending_approval = None
        answer = self.approvals.pop(aid, {}).get("answer") or "timeout"
        self.event("resolved", id=aid)
        print(f"   [approval {aid}] {answer}")
        return answer

    def resolve_approval(self, aid, answer):
        entry = self.approvals.get(aid)
        if entry and not entry["event"].is_set():
            entry["answer"] = answer
            entry["event"].set()

    @staticmethod
    def spoken_approval(text):
        """allow / always / deny for a sentence said while an approval card is up."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        if ALWAYS.search(t):
            return "always"
        if NO.search(t):
            return "deny"
        if ALL.search(t):
            return "all"
        if YES.search(t):
            return "allow"
        return None                                                           # not an answer (a video, someone else talking): ignore it

    def _ask_json(self, prompt):
        """One quick, non-streamed JSON answer from the brain (memory upkeep). Same context size: no model reload."""
        return self._ask(prompt, True)

    def _ask(self, prompt, as_json=False):
        """One non-streamed answer from the brain (background work: memory, research)."""
        body = gpu.body(BRAIN, {"num_ctx": CTX, "temperature": 0.1 if as_json else 0.4, "num_predict": 2500}, stream=False, think=False,
                        messages=[{"role": "user", "content": prompt}])
        if as_json:
            body["format"] = "json"
        req = urllib.request.Request(OLLAMA, json.dumps(body).encode(), {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=600) as resp:
            return json.loads(resp.read())["message"]["content"]

    # ---- research in the background ---------------------------------------------------------------------------------------
    def start_research(self, topic, depth="deep"):
        running = [j for j in self.jobs if j.is_alive()]
        if running:
            return f"error: research on '{running[0].topic}' is still running ({running[0].status}); wait for it to finish"
        job = research_mod.Job(topic, depth, self._ask, self._wait_idle, self._research_done)
        self.jobs = (self.jobs + [job])[-5:]
        job.start()

        def follow():                                                         # the island shows it while it works
            while job.is_alive():
                self.event("activity", id="research", title="Researching", detail=f"{job.topic} · {job.status}",
                           progress=round(max(0.03, job.progress), 2))
                time.sleep(2)
        threading.Thread(target=follow, daemon=True).start()
        minutes = "3 to 6" if job.depth == "deep" else "1 to 3"
        return (f"started {job.depth} research on '{job.topic}' in the background (about {minutes} minutes). Tell the user you're on it, "
                f"that they can keep talking to you, and that the report will open in Chrome when it's ready.")

    def chat_busy(self):
        """True while Solu talks, the user talks, or a card waits for an answer: background work must not use the model then."""
        return self.speaking.is_set() or time.monotonic() < self.chat_priority_until or self.pending_approval is not None

    def task_cancelled(self):
        return bool(self.tasks and self.tasks.in_task() and self.tasks.cancelled())

    def task_say(self, text, listen=False):
        """A task says something (a question, the result): after whatever is being said now. listen: the next sentence is the answer."""
        if not text:
            return
        t0 = time.monotonic()
        while self.chat_busy() and time.monotonic() - t0 < 120:
            time.sleep(0.3)
        if listen:
            self.start_conversation()
        self.respond_text(text)

    def _wait_idle(self):
        while self.speaking.is_set():                                         # the conversation always goes first
            time.sleep(0.3)

    def _research_done(self, job):
        self.event("activity_end", id="research", done=not job.error, title="Research ready", detail=job.title)
        if job.error:
            text = f"Sorry, the research on {job.topic} didn't work out. {job.error[:80]}"
        else:
            text = f"Your research on {job.title} is ready. I've opened it in Chrome. Want me to read you the summary?"
        self._wait_idle()
        self.respond_text(text)

    def latest_job(self):
        done = [j for j in self.jobs if j.status == "done"]
        if done:
            return done[-1].title, done[-1].markdown, done[-1].path
        path, text = research_mod.latest_report()
        if not path:
            return None, "", None
        title = text.splitlines()[0].lstrip("# ").strip() if text else "your last research"
        return title, text, path

    @staticmethod
    def report_section(markdown, name):
        m = re.search(rf"##\s*{name}\s*\n(.+?)(?=\n##\s|\Z)", markdown, re.S | re.I)
        return m.group(1).strip() if m else ""

    @staticmethod
    def for_reading(markdown):
        """Report text as it should be read aloud: no markup, no source numbers, no source list."""
        text = re.split(r"\n##\s*Sources", markdown)[0]
        text = re.sub(r"\s*\[\d+(?:[,\-–\s]*\d+)*\]", "", text)
        text = re.sub(r"^#+\s*(.+)$", r"\1.", text, flags=re.M)
        text = re.sub(r"^[-*•]\s+", "", text, flags=re.M)
        lines, out = [ln.strip() for ln in re.sub(r"[*_`]", "", text).splitlines()], []
        for ln in lines:
            if ln and (not out or ln != out[-1]):                             # a repeated title is read once
                out.append(ln)
        return "\n".join(out)

    def _learn(self, transcript):
        saved = self.memory.learn_from(transcript, self._ask_json)
        for fact in saved:
            print(f"   [memory] learned: {fact}")

    def _load_brain(self):
        try:
            req = urllib.request.Request(OLLAMA.replace("/chat", "/generate"), json.dumps(gpu.body(BRAIN, {"num_ctx": CTX}, prompt="")).encode(),
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
        if re.search(r"\bwhy (is|does|did) (this|it|my (code|build|program|app))( keep)? (fail|failing|crash|crashing|break|breaking|not work)|"
                     r"\bfix (this|the) (error|bug|problem)\b|\bwhat('?s| is) wrong with (this|my) (code|build|program)\b|"
                     r"\btranslate (this|that|it|the (page|text|screen|message|email))\b", t):
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
        self._answer_look(look, text)

    def _answer_look(self, look, text, mode=None):
        """Answer `text` about a captured window or area: explain, translate, help with an error."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        self.event("looking", title=look.title, app=look.app, scope=look.scope, thumb=screen_mod.thumbnail_b64(look))
        ocr = screen_mod.read_text(look)
        question, note = text, SCREEN_NOTE
        if re.fullmatch(r"(look( at (this|that|it|my screen|the screen))?|take a look|have a look|see this|what('?s| is) on my screen|can you see (this|it|that|my screen))", t):
            question = SCREEN_ONLY_LOOK
        if mode == "translate" or re.search(r"\btranslat", t):
            to = re.search(r"\b(?:to|into)\s+(bangla|bengali|english|hindi|urdu|arabic|spanish|french|german|chinese|japanese|korean)\b", t)
            target = f"into {to.group(1).capitalize()}" if to else "into English (or into Bangla if the text is already English)"
            question, note = f"Translate the text on this screen {target}.", TRANSLATE_NOTE.format(target=target)
        elif mode == "explain" and t in ("", "explain this", "look at this"):
            question = "Explain what this part of the screen shows and anything important in it."
        elif re.search(r"\b(error|bug|fail|failing|crash|exception|not working|broken|fix)\b", t):
            note = CODE_NOTE
        translating = question.startswith("Translate the text")
        vlm = self.needs_vision(question, ocr) and not translating
        screen = {"title": look.title, "app": look.app, "scope": look.scope, "text": ocr, "vlm": vlm, "note": note,
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

    # ---- quick capture --------------------------------------------------------------------------------------------------
    def respond_region(self, x, y, w, h, mode="explain"):
        from PIL import ImageGrab
        try:
            self.cancel.clear()
            image = ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True).convert("RGB")
            look = screen_mod.Look("the area you selected", "screen", image, "region")
            look.origin = (x, y)
            question = "Translate this." if mode == "translate" else "Explain this."
            self.start_conversation()
            self.event("heard", text="(the area you showed me)")
            self.event("state", state="thinking")
            self._answer_look(look, question, mode)
        except Exception as e:                                                # noqa: BLE001
            print(f"   (region failed: {e})")
            self.respond_text("Sorry, I couldn't read that part of the screen.")

    # ---- skills that are not a single tool call --------------------------------------------------------------------------
    def special(self, target):
        """A request that a skill handles itself (routines, quiz answers, the briefing, quick capture). A callable, or None."""
        t = " ".join(re.findall(r"[a-z0-9']+", target.lower()))
        task = self._task_skill(target, t)
        if task:
            return task
        if self.quiz:                                                        # a quiz is running: this is the answer
            if re.search(r"\b(stop|end|quit|finish|cancel)\b.*\bquiz\b|\b(stop|enough|that's enough)\b$", t):
                return self.end_quiz
            return lambda: self.quiz_answer(target)
        if re.search(r"\b(stop|end|finish|done|that's it)\b.*\b(taking )?(notes?|recording|transcri\w*)\b|\bstop (the )?(lecture|meeting)\b|\bnotes? (are )?(done|finished)\b", t):
            return lambda: self.respond_text(self._notes_stop_line())
        m = re.search(r"\b(?:start|begin|take|record|make|write)\b.*\b(?:notes?|transcri\w*)\b", t) or re.fullmatch(r"(lecture|meeting|class|video) notes?( please)?", t)
        if m and not self.notes and re.search(r"\b(lecture|meeting|video|class|call|this|notes?)\b", t) and not re.search(r"\b(stop|end|my notes|open|read)\b", t):
            kind = "meeting" if re.search(r"\b(meeting|call|zoom|meet)\b", t) else "video" if "video" in t else "lecture"
            return lambda: self.respond_text(self._notes_start_line(kind))
        defined = self.routines.define(target)
        if defined:
            name, steps = defined
            mine = steps.rstrip(".")
            for a, b in ((r"\bme\b", "you"), (r"\bmy\b", "your"), (r"\bI\b", "you"), (r"\bmine\b", "yours")):   # "tell me" -> "tell you"
                mine = re.sub(a, b, mine)
            return lambda: self.respond_text(f"Okay. Whenever you say {name}, I'll {mine}.")
        name = self.routines.match(target)
        if name:
            return lambda: self.run_routine(name)
        if re.search(r"\b(list|what are) (my |the )?routines\b", t):
            names = list(self.routines.items)
            return lambda: self.respond_text(("Your routines are: " + ", ".join(names) + ".") if names else "You don't have any routines yet.")
        m = re.search(r"\b(?:delete|remove|forget) (?:the |my )?routine (?:called )?(.+)$", t)
        if m:
            gone = self.routines.delete(m.group(1))
            return lambda: self.respond_text(f"Deleted the {gone} routine." if gone else "I don't have a routine like that.")
        if re.search(r"^(good morning|morning( solu)?|morning briefing|brief me|give me (my|a) briefing|what('s| is) (my day|on today|up today))\b", t):
            return self.briefing
        if re.search(r"\b(let me show you|i'll show you|i will show you|look at this (part|area|bit)|translate this (part|area|bit))\b", t):
            mode = "translate" if "translat" in t else "explain"
            return lambda: (self.event("capture", mode=mode), self.respond_text("Okay, drag over the part you mean."))
        return None

    def _task_skill(self, target, t):
        """Starting, steering and answering long tasks by voice. A callable, or None."""
        runner = self.tasks
        if runner is None:
            return None
        say = lambda text: (lambda: self.respond_text(text))
        live = any(x.status in ("planning", "running", "waiting", "paused") for x in runner.tasks)
        if runner.waiting_question():                                         # the task asked something: this is the answer
            if re.search(r"\b(stop|cancel|abort)\b.*\btask\b", t):
                return lambda: self.respond_text(runner.control("cancel"))
            return lambda: (runner.answer(target), self.respond_text("Okay."))
        sched = tasks_mod.parse_schedule(target)
        if sched:
            goal, at, days = sched
            runner.add_schedule(goal, at, days)
            when = "every day" if len(days) == 7 else "on weekdays" if days == [0, 1, 2, 3, 4] else "weekly"
            return say(f"Okay. {when.capitalize()} at {at} I'll {goal.rstrip('.')}.")
        if re.search(r"\b(what('s| is| are)|list|show)\b.*\b(scheduled|schedules|recurring)\b", t):
            return say("Scheduled: " + runner.list_schedules() + ".")
        m = re.match(r"^(?:please )?(?:stop|cancel|remove|delete) (?:doing |the schedule for )?(.+?) every (?:day|morning|evening|night|weekday|week)", t)
        if m:
            gone = runner.remove_schedule(m.group(1))
            return say(f"Okay, I won't do that every day any more." if gone else "I don't have a schedule like that.")
        if live and re.search(r"\b(pause|hold off|freeze)\b.*\b(task|work|that)\b|\bpause the task\b", t):
            return lambda: self.respond_text(runner.control("pause"))
        if re.search(r"\b(resume|continue|carry on|keep going)\b.*\b(task|work)\b|^(continue|carry on|resume) (the )?task", t) and \
                any(x.status == "paused" for x in runner.tasks):
            return lambda: self.respond_text(runner.control("resume"))
        if live and re.search(r"\b(stop|cancel|abort|kill|end)\b.*\b(task|that work|the work)\b|\bstop working on (it|that|this)\b", t):
            return lambda: self.respond_text(runner.control("cancel"))
        if re.search(r"\b(clear|remove|delete|forget)\b.*\b(finished|done|old|completed) tasks?\b", t):
            return lambda: ([runner.control("remove", x.id) for x in list(runner.tasks) if x.status in ("done", "failed", "cancelled")],
                            self.respond_text("Cleared the finished tasks."))
        if re.search(r"\b(list|show|what)\b.*\btasks\b", t):
            return lambda: self.respond_text(runner.list_text() + ".")
        if runner.tasks and (re.search(r"\btask\b.*\b(status|going|progress|doing|done yet)\b|\bhow('s| is) the task\b|\bwhat are you (working on|doing)\b|\bany progress\b", t)):
            return lambda: self.respond_text(runner.status_text())
        m = re.match(r"^(?:please )?(?:start|begin|run|create|set up|give you|do)(?: a| the| this)?(?: long| big| background)? task[:,]?\s+(.{8,})$", target.strip(), re.I) or \
            re.match(r"^(?:your )?(?:task|goal) (?:is|:)\s*(.{8,})$", target.strip(), re.I) or \
            re.match(r"^(?:i want you to|i need you to|can you|could you) (?:go and |now )?(?:work on|take care of|figure out|handle) (.{8,}?)(?: (?:by yourself|on your own|in the background))?$", target.strip(), re.I)
        if m:
            goal = m.group(1).strip().rstrip(".")
            return lambda: (runner.submit(goal), self.respond_text("Okay, I'll plan that and show you the plan to approve."))
        return None

    def run_routine(self, name):
        steps = self.routines.items[name]
        print(f"   [routine] {name}: {steps}")
        self.respond_routine = True
        try:
            self.cancel.clear()
            self._speaking_reported = False
            self.speaking.set()
            self.allow_all_next = True                                      # the user wrote this routine: its safe steps need no cards
            self._agent(f"Do all of these now, one after another: {steps}")
        finally:
            self._after_speaking()

    # ---- study buddy ------------------------------------------------------------------------------------------------------
    def start_quiz_from(self, source=""):
        src = (source or "").strip()
        text, where = "", ""
        if src and not re.search(r"\b(this|screen|page|pdf|document|window|it)\b", src.lower()):
            text, where = actions.research(src), src                         # a topic: learn it from the web first
        else:
            look = screen_mod.look_at_foreground()
            if look.blocked:
                return "error: " + look.blocked
            path = None
            if look.title.lower().split(" - ")[0].strip().endswith(".pdf"):  # a PDF reader: read the whole file, not just the page
                name = look.title.split(" - ")[0].strip()
                found = actions.search_files(name).splitlines()
                path = next((ln.replace("[folder] ", "") for ln in found if ln.lower().endswith(".pdf")), None)
            if path and os.path.exists(path):
                text, where = skills.pdf_text(path), os.path.basename(path)
            else:
                text, where = screen_mod.read_text(look), look.title
        if len(text) < 200:
            return "error: there isn't enough text to make a quiz from; open the page or PDF first, or name a topic"
        data = json.loads(self._ask(skills.QUIZ.format(n=5, text=text[:9000]), True))
        questions = [q for q in data.get("questions", []) if q.get("q") and q.get("a")][:5]
        if not questions:
            return "error: I couldn't make questions from that"
        self.quiz = skills.Quiz(questions, data.get("topic") or where)
        return (f"Quiz ready on {self.quiz.topic}: {len(questions)} questions. Tell the user you'll quiz them, then ask question 1 "
                f"exactly as written: {questions[0]['q']}")

    def quiz_answer(self, answer):
        quiz = self.quiz
        q = quiz.current
        if not q:
            return self.end_quiz()
        try:
            verdict = json.loads(self._ask(skills.GRADE.format(q=q["q"], a=q["a"], answer=answer), True))
        except Exception:                                                     # noqa: BLE001
            verdict = {"correct": False, "feedback": f"The answer was: {q['a']}"}
        if verdict.get("correct"):
            quiz.score += 1
        else:
            quiz.missed.append(q.get("point") or q["q"])
        quiz.index += 1
        line = verdict.get("feedback", "")
        if quiz.current:
            line += f" Question {quiz.index + 1}: {quiz.current['q']}"
            self.respond_text(line)
        else:
            self.respond_text(line)
            self.end_quiz()

    def end_quiz(self):
        quiz, self.quiz = self.quiz, None
        if not quiz:
            return
        asked = max(1, quiz.index)
        text = f"That's the quiz! You got {quiz.score} out of {asked}."
        if quiz.missed:
            text += " Worth reviewing: " + ", ".join(quiz.missed[:3]) + "."
            self.memory.add(f"In a quiz on {quiz.topic}, the user found these hard: {', '.join(quiz.missed[:3])}.", "plan", "learned")
        else:
            text += " Perfect, nice work!"
            self.event("cheer")
        self.respond_text(text)

    # ---- lecture / meeting notes ----------------------------------------------------------------------------------------------
    def notes_start(self, kind="lecture"):
        if self.notes and self.notes.is_alive():
            return "error: I'm already taking notes; say stop taking notes when it's over"
        self.notes = skills.NoteTaker(self, kind)
        self.notes.start()
        self.event("activity", id="notes", title="Taking notes", detail=f"{kind} · listening to your PC")
        return f"started taking notes of the {kind} from the PC's sound. Tell the user you're listening and they should tell you when it's over."

    def _notes_start_line(self, kind):
        result = self.notes_start(kind)
        if result.startswith("error"):
            return "I'm already taking notes."
        return f"Okay, I'm listening to the {kind} on your PC. Tell me when it's over."      # no quoted phrase for the echo filter to trip on

    def _notes_stop_line(self):
        if not self.notes:
            return "I'm not taking notes right now."
        self.notes_stop()
        return "Okay, writing your notes now. They'll open in Chrome in a minute."

    def notes_stop(self):
        if not self.notes:
            return "error: I'm not taking notes right now"
        notes, self.notes = self.notes, None

        def finish():
            try:
                path, title, markdown = notes.finish()
                self.event("activity_end", id="notes", done=True, title="Notes ready", detail=title)
                self._wait_idle()
                self.respond_text(f"Your notes on {title} are ready. I've opened them in Chrome.")
                self.jobs.append(type("Done", (), {"status": "done", "title": title, "markdown": markdown, "path": path,
                                                  "started": time.time(), "is_alive": lambda self: False})())
            except Exception as e:                                            # noqa: BLE001
                self.event("activity_end", id="notes")
                self._wait_idle()
                self.respond_text(f"Sorry, I couldn't write the notes: {str(e)[:100]}")
        threading.Thread(target=finish, daemon=True).start()
        return "stopped recording; the notes are being written and will open in Chrome in a minute. Tell the user that."

    # ---- morning briefing ------------------------------------------------------------------------------------------------------
    def briefing(self):
        self.cancel.clear()
        self._speaking_reported = False
        self.speaking.set()
        try:
            facts = [time.strftime("It is %A %d %B, %I:%M %p")]                # (not uptime or memory: that's no briefing)
            w = skills.weather()
            if w:
                facts.append("Weather: " + w)
            unread = skills.unread_summary(self.context)
            if unread:
                facts.append("Notifications waiting: " + unread)
            plans = [m["text"] for m in self.memory.items if m["kind"] == "plan" or skills.dates_in(m["text"])]
            if plans:
                facts.append("Plans the user mentioned: " + "; ".join(plans[-5:]))
            title, _, _ = self.latest_job()
            if title:
                try:
                    news = actions.search_results(f"{title} news")[:1]
                    if news:
                        facts.append(f"A recent headline about something they researched ({title}): {news[0][0]} - {news[0][2][:200]}")
                except Exception:                                             # noqa: BLE001
                    pass
            name = self._user_name()
            prompt = ("Give the user a warm, short spoken briefing (4 to 6 sentences) from these facts. Greet them for the time of day"
                      + (f" by name ({name})" if name else " without using any name (you don't know it)") +
                      "; mention the weather, anything waiting for them and their plans; end with one encouraging line. "
                      "Only mention rain if the chance is above 40%. Use only these facts:\n- " + "\n- ".join(facts) +
                      "\nWhat you know about the user:\n- " + "\n- ".join(self.memory.relevant("plans today")))
            self.history.append({"role": "user", "content": "Good morning! Brief me."})
            text = self._ask(prompt).strip()
            self.history.append({"role": "assistant", "content": text})
        except Exception as e:                                                # noqa: BLE001
            text = f"Good morning! I couldn't put the whole briefing together just now ({str(e)[:60]})."
        finally:
            self.speaking.clear()
        self.respond_text(text)

    # ---- smart messaging ----------------------------------------------------------------------------------------------------
    def smart_send(self, to, message, app=""):
        """Find the person (memory, WhatsApp search or Gmail's suggestions), show the card, then send."""
        to, app = to.strip(), (app or "").lower()
        address = actions.email_address(to)
        known = " ".join(self.memory.relevant(to))
        if not address and app != "whatsapp":                                 # an email address remembered for this person?
            m = re.search(r"[A-Za-z0-9._%+\-]+@[A-Za-z0-9\-.]+\.[A-Za-z]{2,}", known) if to.lower().split()[0] in known.lower() else None
            if m:
                address = m.group(0)
        if not app:
            app = "email" if address or "email" in known.lower() or "gmail" in known.lower() else "whatsapp"
        if app in ("email", "gmail", "mail"):
            if not address:
                address, note = skills.gmail_lookup(to)
                if not address:
                    return f"error: {note}. Ask the user for {to}'s email address."
            subject = message.split(".")[0][:60] if len(message) > 80 else "Message from " + (self._user_name() or "me")
            answer = self.ask_permission("send_email", {"to": address, "subject": subject, "body": message})
            if answer not in ("allow", "always", "all"):
                return "the user said no. Do not send it; just say okay."
            result = actions.send_gmail(address, subject, message)
            self.memory.add(f"{to.title()}'s email address is {address}.", "person", "learned")
            return result
        chat, note = skills.whatsapp_open_chat(to)
        if not chat:
            return f"error: {note}"
        answer = self.ask_permission("whatsapp", {"to": chat, "text": message})
        if answer not in ("allow", "always", "all"):
            return "the user said no. Do not send it; just say okay."
        skills.whatsapp_type_and_send(message)
        self.memory.add(f"The user messages {chat} on WhatsApp.", "person", "learned")
        return f"sent to {chat} on WhatsApp"

    def _user_name(self):
        for m in self.memory.items:
            if m["kind"] == "name":
                return m["text"].rsplit(" ", 1)[-1].rstrip(".")
        return ""

    # ---- listening ----------------------------------------------------------------------------
    def is_echo(self, text):
        norm = lambda t: " ".join("".join(c if c.isalnum() or c == " " else " " for c in t.lower()).split())
        heard = norm(text)
        if not heard:
            return False
        if re.search(r"\b(stop|end|finish|done)\b.*\b(notes?|recording|quiz|research|timer)\b|^(stop|enough|cancel|yes|no)\b", heard):
            return False                                                      # a command is never "my own voice", even if I just quoted it
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
            self.conversation_from = len(self.history)
            self.event("state", state="listening")

    def end_conversation(self):
        if self.in_conversation:
            self.in_conversation = False
            transcript = [(m["role"], m["content"]) for m in self.history[self.conversation_from:]
                          if m["role"] in ("user", "assistant") and m["content"].strip()]
            if transcript:                                                    # look back once, quietly, for anything worth keeping
                threading.Thread(target=self._learn, args=(transcript,), daemon=True).start()
            self.history = self.recent(10, plain=True)[-4:]                   # a fresh start next time, with a little continuity
            self.conversation_from = len(self.history)
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
        if re.search(r"\b(forget|reset|clear)\b.*\b(allowed|permissions?|trust(ed)?|always)\b", t):
            actions.forget_trust()
            return "Okay, I'll ask before everything again."
        # ---- research reports ----
        if re.search(r"\bhow('s| is) (the |my |that )?(research|report)( going)?\b|\b(is|are) (the |my )?(research|report) (done|ready|finished)\b", t):
            running = [j for j in self.jobs if j.is_alive()]
            if running:
                j = running[0]
                return f"Still working on {j.title}. I'm {j.status} right now, about {int(100 * j.progress)} percent done."
            title, _, _ = self.latest_job()
            return f"Nothing is running. Your last report was {title}." if title else "There's no research going on right now."
        recent_report = any(j.status == "done" and time.time() - j.started < 3600 for j in self.jobs)
        whole = re.search(r"\bread (me )?(the |that )?(whole|full|entire|complete) (report|research|doc|document|thing)\b", t)
        summary = re.search(r"\bread (me )?(the |that )?(summary|report|research)\b|\bwhat did (you|the research|the report) (find|say)\b", t) or \
            (recent_report and re.fullmatch(r"(yes|yeah|sure|ok|okay|please)?\s*(please )?(read it|read that|read it to me|yes read it|go ahead)( please)?", t))
        if whole or summary:
            title, markdown, _ = self.latest_job()
            if not markdown:
                return "I don't have a research report yet. Ask me to do deep research on something."
            if whole:
                return self.for_reading(markdown)
            part = self.report_section(markdown, "Summary") or self.for_reading(markdown)[:900]
            return f"Here's the summary of {title}. " + self.for_reading(part)
        if re.search(r"\bopen (the |my |that )?(last |latest |previous )?(report|research)\b", t):
            _, _, path = self.latest_job()
            if not path:
                return "I don't have a research report yet."
            research_mod.open_report(path)
            return "Opened it in Chrome."
        # ---- memory ----
        if re.search(r"\b(forget|erase|delete|clear|wipe)\b.*\b(everything|all of it|all about me|your memory|all memories|all your memories)\b", t):
            self.memory.clear()
            return "Okay, I've forgotten everything about you. It's a fresh start."
        if re.search(r"\bwhat (do|did) you (remember|know) about me\b|\bwhat do you remember\b|\bwhat('s| is) in your memory\b", t):
            facts, total = self.memory.summary(5)
            if not facts:
                return "I don't remember anything about you yet. Tell me something, or say remember that, and I'll keep it."
            return "Here's what I remember: " + " ".join(memory_mod.spoken(f) for f in facts) + (f" And {total - 5} more things." if total > 5 else "")
        m = re.match(r"^(?:please |can you |could you )?forget (?:that |about |the fact that )?(.+)$", t)
        if m and not re.search(r"\b(allowed|permission)", t):
            gone = self.memory.forget(m.group(1))
            return f"Okay, I've forgotten that {memory_mod.spoken(gone)[0].lower()}{memory_mod.spoken(gone)[1:]}" if gone else \
                "I don't have anything like that in my memory."
        m = re.search(r"(?:^|\b(?:please|can you|could you|and)\s+)(?:remember|don't forget|keep in mind|note)\s+(?:that\s+)?(.{6,})$", t)
        if m and not re.search(r"\b(do|did|can't|cant|don't|dont) (you|i) remember\b|\bremember when\b", t):
            said = re.search(r"(?:remember|don't forget|keep in mind|note)\s+(?:that\s+)?(.{6,})$", text.strip(), re.I)
            fact = (said.group(1) if said else m.group(1)).strip().rstrip(".!")
            self.memory.add(f"The user told Solu: {fact}.", "fact", "told")
            return "Got it, I'll remember that."
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
        with self.stt_lock:
            segs, _ = self.stt.transcribe(audio, language=self.args.lang, beam_size=1, vad_filter=False,
                                          condition_on_previous_text=False, initial_prompt="Hey Solu.")
            segs = list(segs)
        if not segs or max(s.no_speech_prob for s in segs) > 0.6:
            return ""
        return " ".join(s.text.strip() for s in segs).strip()

    def transcribe_long(self, audio):
        """A stretch of a lecture or meeting (any language Whisper knows), for the note taker."""
        with self.stt_lock:
            segs, _ = self.stt.transcribe(audio, beam_size=1, vad_filter=True, condition_on_previous_text=True)
            return " ".join(s.text.strip() for s in segs if s.no_speech_prob < 0.7).strip()

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
                if self.need_pa_refresh:                                      # a device appeared since PortAudio started: re-read the list
                    self.need_pa_refresh = False
                    try:
                        sd._terminate()
                        sd._initialize()
                        self.audio_generation += 1                            # the speaker stream died with it: the player opens a new one
                    except Exception as e:                                    # noqa: BLE001
                        print(f"   (could not refresh the audio devices: {str(e)[:80]})")
                    self.check_devices(force=True)
                if self.paused:                                               # privacy key: the microphone stays closed
                    self.mic_level = 0.0
                    if stop and stop():
                        return
                    time.sleep(0.3)
                    continue
                device, q, epoch = self.in_dev, queue.Queue(), self.in_epoch
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
                    while device == self.in_dev and epoch == self.in_epoch and not self.paused:
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
            if idle_timeout and self.in_conversation and not self.quiz and not self.speaking.is_set() and not talking and (worker is None or not worker.is_alive()):
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
            if p > 0.5:
                self.chat_priority_until = time.monotonic() + 2.0              # somebody talks: tasks leave the model alone
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
                if silence_ms >= limit and spec is not None and (worker is None or not worker.is_alive() or self.pending_approval):
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
                    if self.pending_approval:                                 # an approval card is up: this is the answer
                        answer = self.spoken_approval(target) if target else None
                        print(f"you: {target}   -> approval: {answer or 'not an answer, ignored'}")
                        if answer:
                            self.event("heard", text=target)
                            self.resolve_approval(self.pending_approval, answer)
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
                    skill = self.special(target)
                    if skill:
                        worker = threading.Thread(target=skill, daemon=True)
                        worker.start()
                        continue
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
