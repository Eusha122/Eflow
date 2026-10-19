"""Live test of Solu's real mouse tools (click by reading the screen, scroll) in a throwaway window this test creates.
It never touches another app: every action first checks that the test window is the one in front.
   python tests/live_mouse_test.py
"""
import ctypes
import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import actions  # noqa: E402

TITLE = "Solu live mouse test"
STATE = os.path.join(tempfile.gettempdir(), "solu_live_mouse_test.json")
user32 = ctypes.windll.user32

WINDOW = r'''
import json, sys, tkinter as tk
state_path, title = sys.argv[1], sys.argv[2]
state = {"clicked": None, "scroll": 0.0}
def save(): json.dump(state, open(state_path, "w"))
root = tk.Tk(); root.title(title); root.geometry("900x620+300+150"); root.configure(bg="#202124")
cards = tk.Frame(root, bg="#202124"); cards.pack(pady=20)
for i, name in enumerate(["Eusha", "Eusha ff", "Eusha Ibna Akbor", "Ivy Closet"]):
    b = tk.Label(cards, text=name, font=("Segoe UI", 16), fg="white", bg="#35363a", width=16, height=3)
    b.grid(row=0, column=i, padx=8)
    b.bind("<Button-1>", lambda e, n=name: (state.update(clicked=n), save()))
text = tk.Text(root, font=("Segoe UI", 12), bg="#2b2c30", fg="white", height=18)
text.pack(fill="both", expand=True, padx=20, pady=10)
text.insert("1.0", "\n".join(f"line {i}" for i in range(400)))
def poll():
    state["scroll"] = round(text.yview()[0], 4); save(); root.after(100, poll)
root.after(300, poll); root.lift(); root.attributes("-topmost", True); root.after(600, lambda: root.attributes("-topmost", False))
root.focus_force(); root.mainloop()
'''


def front_title():
    hwnd = user32.GetForegroundWindow()
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, buf, 256)
    return buf.value


def state():
    try:
        return json.load(open(STATE))
    except (OSError, ValueError):
        return {}


def main():
    if os.path.exists(STATE):
        os.remove(STATE)
    script = os.path.join(tempfile.gettempdir(), "solu_live_mouse_window.py")
    open(script, "w").write(WINDOW)
    proc = subprocess.Popen([sys.executable, script, STATE, TITLE])
    hands, failed = actions.Hands(None), 0
    try:
        for _ in range(40):
            if front_title() == TITLE and state():
                break
            time.sleep(0.25)
        if front_title() != TITLE:
            print(f"ABORT: the test window is not in front (front: {front_title()!r}); nothing was clicked")
            return 1

        result = hands.do_click_text("Usha Ibn Akbar")                         # how Whisper spelled the name
        time.sleep(0.4)
        ok = state().get("clicked") == "Eusha Ibna Akbor"
        print(f"[{'PASS' if ok else 'FAIL'}] click by reading the screen: {result!r} -> clicked {state().get('clicked')!r}")
        failed += not ok

        if front_title() != TITLE:
            print("ABORT: the test window lost the front; not scrolling")
            return 1
        before = state()["scroll"]
        result = hands.do_scroll("down", 0.3)
        time.sleep(0.5)
        after = state()["scroll"]
        ok = after > before
        print(f"[{'PASS' if ok else 'FAIL'}] scroll down a bit: {result!r} -> position {before} -> {after}")
        failed += not ok
    finally:
        proc.terminate()
    print("all passed" if not failed else f"{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
