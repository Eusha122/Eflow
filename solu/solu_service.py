"""Solu as a hidden helper of the Eflow island. It is awake and listening for its name from the moment it has loaded.

Eflow starts this process, reads one JSON event per line from its stdout and writes one JSON command per
line to its stdin. Everything human-readable goes to stderr (Eflow keeps it in %LOCALAPPDATA%\\Eflow\\solu.log).

events   {"event": "state", "state": "loading" | "ready" | "listening" | "thinking" | "speaking"}
         {"event": "heard", "text": ...}   {"event": "said", "text": ...}   {"event": "level", "v": 0..1}
         {"event": "error", "message": ...}   {"event": "idle"}
commands {"cmd": "talk"}        start a hands-free conversation (the microphone opens), or end it
         {"cmd": "stop"}        stop talking right now
         {"cmd": "voice", "name": "cozy"}      change the voice (Solu says hello in it)
         {"cmd": "headphones", "on": true}     headphones: you can talk over Solu; speakers: it never hears itself
         {"cmd": "ask", "text": "..."}         answer a typed question (no microphone needed)
         {"cmd": "approval", "id": 3, "answer": "allow" | "always" | "deny"}   the user's answer on the approval card
         (events: {"event": "approve", id, action, detail, risky, always} shows the card, {"event": "resolved", id} removes it)
         {"cmd": "task", "op": "pause" | "resume" | "cancel" | "remove", "id": "..."}   steer a long task (events: tasks, activity)
         {"cmd": "screen", "on": true}         allow or forbid "look at this" (events: {"event": "looking", title, app, thumb})
         {"cmd": "quit"}
The microphone is only ever open between "talk" and the end of the conversation.
"""
import argparse
import json
import os
import queue
import sys
import threading
import time

OUT = sys.stdout
sys.stdout = sys.stderr                                      # prints in solu_chat go to the log, not into the event stream
os.chdir(os.path.dirname(os.path.abspath(__file__)))         # models/ is relative to this folder
# An installed Eflow runs this on the embeddable Python, whose ._pth file fixes sys.path outright: neither this
# folder nor the working directory is on it, so every import below would be a ModuleNotFoundError. A development
# checkout runs on a venv, which adds this folder itself - which is why only installed copies ever saw it.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import solu_chat  # noqa: E402
import gpu  # noqa: E402
from config import BRAIN, FAST  # noqa: E402
from voice_presets import PRESETS  # noqa: E402

emit_lock = threading.Lock()


def emit(_event, **kw):                                      # (not "name": the setting events have a field called name)
    with emit_lock:
        OUT.write(json.dumps({"event": _event, **kw}) + "\n")
        OUT.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", default="cozy")
    ap.add_argument("--headphones", action="store_true")
    ap.add_argument("--no-screen", action="store_true", help="Solu may not look at the screen")
    ap.add_argument("--idle", type=float, default=15.0, help="seconds of silence that end a conversation")
    opts = ap.parse_args()

    emit("state", state="loading")
    args = argparse.Namespace(voice=opts.voice, lang="en", headphones=opts.headphones, endpoint=600, mic=None, wav=None, service=True, no_screen=opts.no_screen)
    solu = solu_chat.Solu(args)
    solu.event = emit
    solu.wake_mode = True                                     # wait for its name; a conversation starts when it hears it
    solu.in_conversation = False
    solu.check_devices(force=True)

    commands = queue.Queue()

    def read_stdin():
        for line in sys.stdin:
            try:
                commands.put(json.loads(line))
            except ValueError:
                pass
        commands.put({"cmd": "quit"})                        # Eflow closed the pipe: it is gone, so are we

    threading.Thread(target=read_stdin, daemon=True).start()

    def listen_forever():
        while True:
            try:
                solu.run(idle_timeout=opts.idle)
            except Exception as e:                            # noqa: BLE001
                emit("error", message=f"The microphone stopped: {str(e)[:100]}")
                time.sleep(2)

    threading.Thread(target=listen_forever, daemon=True).start()
    emit("state", state="ready")                              # awake: listening for its name

    while True:
        cmd = commands.get()
        name = cmd.get("cmd")
        if name == "quit":
            for model in (BRAIN, FAST):                               # Solu off or Eflow closing: give the graphics card back now
                gpu.unload(model)
            os._exit(0)
        elif name == "talk":                                  # the hotkey: start a conversation, or end the running one
            if solu.in_conversation:
                solu.cancel.set()
                solu.end_conversation()
            else:
                solu.cancel.clear()
                solu.start_conversation()
        elif name == "stop":
            solu.cancel.set()
            while not solu.audio_out.empty():
                try:
                    solu.audio_out.get_nowait()
                except queue.Empty:
                    break
        elif name == "voice" and cmd.get("name") in PRESETS:
            solu.args.voice = cmd["name"]
            if not solu.speaking.is_set():                    # a short hello so you can hear the new voice
                solu.cancel.clear()
                threading.Thread(target=solu.respond_text, args=("Hi! This is my new voice.", False), daemon=True).start()
        elif name == "headphones":
            solu.half_duplex = not bool(cmd.get("on"))
        elif name == "screen":
            solu.screen_enabled = bool(cmd.get("on"))
        elif name == "task" and solu.tasks:                                   # pause / resume / stop / remove, from the island
            threading.Thread(target=solu.tasks.control, args=(str(cmd.get("op", "")), cmd.get("id") or None), daemon=True).start()
        elif name == "say" and cmd.get("text"):                               # the app has something to say (focus timer)
            def say_when_free(text=cmd["text"]):
                solu._wait_idle()
                solu.respond_text(text)
            threading.Thread(target=say_when_free, daemon=True).start()
        elif name == "pause":                                                 # privacy key
            solu.paused = bool(cmd.get("on"))
            print(f"   [privacy] microphone {'closed' if solu.paused else 'open again'}")
            if solu.paused:
                solu.cancel.set()
                solu.end_conversation()
        elif name == "region":                                                # quick capture
            threading.Thread(target=solu.respond_region, args=(int(cmd["x"]), int(cmd["y"]), int(cmd["w"]), int(cmd["h"]),
                                                              cmd.get("mode", "explain")), daemon=True).start()
        elif name == "reminder_reply":                                       # Eflow added (or could not add) a reminder
            solu.reminder_replies.put(cmd.get("text", ""))
        elif name == "context":
            solu.context.update(cmd.get("data") or {})                        # notifications and agents arrive separately
        elif name == "approval" and cmd.get("answer") in ("allow", "always", "deny", "all"):
            solu.resolve_approval(int(cmd.get("id", 0)), cmd["answer"])
        elif name == "ask" and cmd.get("text") and not solu.speaking.is_set():
            emit("heard", text=cmd["text"])
            emit("state", state="thinking")
            threading.Thread(target=solu.handle, args=(cmd["text"],), daemon=True).start()


if __name__ == "__main__":
    main()
