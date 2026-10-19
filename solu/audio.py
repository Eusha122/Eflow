"""Hearing and speaking: the microphone and speaker devices (following the island's choice and Windows' default),
voice activity detection, speech-to-text with Whisper and the playback queue for Kokoro's voice."""
import ctypes
import json
import os
import queue
import re
import time
import wave

import numpy as np
import onnxruntime as ort

from voice_presets import speak


SR_IN, FRAME = 16000, 512                       # 32 ms frames, what Silero expects
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


class AudioMixin:
    """The microphone, the speaker and the voice. Mixed into Solu."""

    # ---- devices ---------------------------------------------------------------------------------------------
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


    # ---- speaking --------------------------------------------------------------------------------------------
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
        self.saying = (self.saying + [text])[-6:]                             # sentences on their way out: an echo of them is not the user
        audio, _ = speak(self.tts, self.args.voice, text)
        if not self.cancel.is_set():
            self.audio_out.put(audio)
            self.audio_out.put(np.zeros(int(24000 * 0.08), np.float32))

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
        if self.cancel.is_set():                                              # drop what was not played; never swap the queue,
            while not self.audio_out.empty():                                 # the player thread keeps waiting on this one
                try:
                    self.audio_out.get_nowait()
                except queue.Empty:
                    break
        if self.half_duplex and not self.args.wav:
            self.quiet_until = max(self.quiet_until, time.perf_counter() + 1.2)   # a phone mic adds delay: its echo arrives late
        self.speaking.clear()
        self.event("level", v=0.0)
        self.event("state", state="listening" if self.in_conversation else "ready")
        after, self._after = self._after, None
        if after:
            after()


    # ---- hearing ---------------------------------------------------------------------------------------------
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
