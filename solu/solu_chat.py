"""Solu, the voice companion that lives in Eflow's island. This file puts it together and runs it from a console.

    python solu_chat.py                  speaker mode: Solu does not listen while it talks (it never hears itself)
    python solu_chat.py --headphones     headphones: you can interrupt Solu by talking over it
    python solu_chat.py --wake           wait for its name, like the island does
    python solu_chat.py --wav in.wav     feed a recording instead of the microphone and save the reply (latency test)

Eflow runs it through solu_service.py (JSON lines on stdin/stdout). The pieces, one job per module:

    listening.py    microphone loop, wake word, conversations            audio.py        devices, speech-to-text, playback
    intents.py      fixed commands (play, open, click, settings, ...)     brain.py        the language model and its tools
    approvals.py    approval cards                                        screen_help.py  questions about the screen
    jobs.py         deep research, task hooks                             skill_runner.py routines, quiz, notes, briefing
    config.py       models and the model server                           prompts.py      what the model is told
    actions.py      the tools themselves (Hands)                          tasks.py        long background tasks
"""
import argparse
import glob
import os
import queue
import sys
import threading

site = os.path.join(os.path.dirname(sys.executable), "..", "Lib", "site-packages", "nvidia")
for d in glob.glob(os.path.join(site, "*", "bin")):                      # CUDA libraries from pip, for Whisper on the GPU
    os.add_dll_directory(os.path.abspath(d))
    os.environ["PATH"] = os.path.abspath(d) + os.pathsep + os.environ["PATH"]

from faster_whisper import WhisperModel  # noqa: E402
from kokoro_onnx import Kokoro  # noqa: E402
import numpy as np  # noqa: E402

from approvals import ApprovalsMixin  # noqa: E402
from audio import AudioMixin  # noqa: E402
from audio import Vad  # noqa: E402
from brain import BrainMixin  # noqa: E402
from config import BRAIN, CTX, OLLAMA  # noqa: E402
from jobs import JobsMixin  # noqa: E402
from listening import ListeningMixin  # noqa: E402
from screen_help import ScreenHelpMixin  # noqa: E402
from skill_runner import SkillsMixin  # noqa: E402
from voice_presets import PRESETS  # noqa: E402
from voice_presets import speak  # noqa: E402
import actions  # noqa: E402
import intents  # noqa: E402
import memory as memory_mod  # noqa: E402
import skills  # noqa: E402
import tasks as tasks_mod  # noqa: E402


class Solu(ListeningMixin, AudioMixin, BrainMixin, ApprovalsMixin, ScreenHelpMixin, JobsMixin, SkillsMixin):
    """One Solu: the state every part shares lives here; the behaviour lives in the mixins (one module each)."""

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
        self.reminder_replies = queue.Queue()                                # Eflow's answers to "remind me ..." (it owns the reminders)
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
        self.saying = []                                                     # the sentences of the answer being spoken right now
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

    def handle(self, text):
        """Answer one request (already addressed to Solu): a fixed command if one matches, otherwise the brain.
        Blocks until Solu has answered, so callers run it on a worker thread."""
        handler = intents.match(self, text)
        if handler is not None:
            handler()
        else:
            self.respond(text)


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
