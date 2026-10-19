"""The listening loop: turn-taking on the microphone, its name ("Solu"), echo of its own voice, conversations."""
import difflib
import random
import re
import threading
import time

import numpy as np

from audio import SR_IN


UNFINISHED = {"and", "but", "so", "because", "or", "the", "a", "an", "to", "of", "um", "uh", "like", "then",
              "if", "when", "that", "with", "for", "my", "is", "are", "i", "its", "it's", "also", "well"}
JUNK = {"thank you", "thanks for watching", "you", "bye", "thanks", "okay"}   # typical Whisper inventions on noise


class ListeningMixin:
    """The microphone loop and the wake word. Mixed into Solu."""

    # ---- its name --------------------------------------------------------------------------------------------
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

    def is_echo(self, text):
        norm = lambda t: " ".join("".join(c if c.isalnum() or c == " " else " " for c in t.lower()).split())
        heard = norm(text)
        if not heard:
            return False
        if re.search(r"\b(stop|end|finish|done)\b.*\b(notes?|recording|quiz|research|timer)\b|^(stop|enough|cancel|yes|no)\b", heard):
            return False                                                      # a command is never "my own voice", even if I just quoted it
        for reply in self.spoken + self.saying:
            said = norm(reply)
            if heard in said:                                                 # a piece of what we just said
                return True
            if len(heard.split()) >= 3 and difflib.SequenceMatcher(None, heard, said).find_longest_match(0, len(heard), 0, len(said)).size >= 0.7 * len(heard):
                return True
        return False


    # ---- conversations ---------------------------------------------------------------------------------------
    def start_conversation(self):
        if not self.in_conversation:
            self.in_conversation = True
            self.conversation_from = len(self.history)
            self.event("state", state="listening")

    def end_conversation(self):
        if self.in_conversation:
            self.in_conversation = False
            transcript = [(m["role"], re.sub(r"\n?\[Screen:.*", "", m["content"], flags=re.S).strip())   # the user's words only:
                          for m in self.history[self.conversation_from:]                               # screen text is not about them
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


    # ---- the loop --------------------------------------------------------------------------------------------
    def run(self, stop=None, idle_timeout=None):
        mode = "headphones: you can talk over Solu" if not self.half_duplex else "speakers: Solu listens only when it is not talking"
        print(f"ready. voice mode: {self.args.voice}. {mode}. {'(feeding ' + self.args.wav + ')' if self.args.wav else 'talk to Solu!'}")
        pre = []                                                               # rolling pre-roll so the first word is kept
        speech, silence_ms, talking, waited, spec = [], 0, False, 0, None
        voiced, barge = 0, None                                                # voice frames heard / the "is that really the user?" check
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
                if not talking:
                    talking, speech, spec, voiced, barge = True, list(pre), None, 0, None
                voiced += 1
                silence_ms, waited, spec = 0, 0, None                           # they kept talking: drop the guess
            elif talking:
                silence_ms += 32
            if talking:
                speech.append(frame)
                if self.speaking.is_set() and not self.half_duplex:            # a voice while Solu talks: the user, or Solu's own echo?
                    if barge is None and voiced >= 18:                         # ~0.6 s of voice: check what was said before stopping
                        barge = {"text": None}
                        threading.Thread(target=lambda b=barge, a=np.concatenate(speech): b.update(text=self.transcribe(a)), daemon=True).start()
                    elif barge is not None and barge["text"] is not None and not barge.get("checked"):
                        barge["checked"] = True
                        heard = barge["text"].strip()
                        if len(heard.split()) >= 2 and not self.is_echo(heard):
                            self.cancel.set()                                 # barge-in: the user really talks over Solu
                            while not self.audio_out.empty():
                                self.audio_out.get_nowait()
                            print(f"   (interrupted by you: {heard})")
                        else:                                                 # its own voice through the speakers: keep talking
                            talking, speech, silence_ms, voiced, barge = False, [], 0, 0, None
                            self.vad.reset()
                            continue
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
                        worker = threading.Thread(target=self.respond_text, args=(random.choice(["Yes?", "I'm here.", "Listening."]), False), daemon=True)
                        worker.start()
                        continue
                    print(f"you: {target}   (speech-to-text {stt_ms:.0f} ms)")
                    self.event("heard", text=target)
                    self.event("state", state="thinking")
                    worker = threading.Thread(target=self.handle, args=(target,), daemon=True)   # a fixed command, or the brain
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
