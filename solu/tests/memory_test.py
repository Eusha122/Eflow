"""Unit tests for memory.py (no model, no microphone): python tests/memory_test.py"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import memory  # noqa: E402

failed = 0


def check(name, ok):
    global failed
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    failed += not ok


path = os.path.join(tempfile.mkdtemp(), "memory.json")

m = memory.Memory(path)
m.add("The user's name is Eusha Ibna Akbor.", "name", "told")
check("a learned name never replaces a told one", not m.add("The user's name is Yush.", "name", "learned") and m.user_name() == "Eusha Ibna Akbor")
check("a told name replaces a told name", m.add("The user's name is Eusha.", "name", "told") and m.user_name() == "Eusha")

m.add("The user's favorite playlist is maths.", "preference")
check("favourites are found by kind", m.find_favorite("playlist") == "maths")
m.add("The user's favorite playlist is chill.", "preference")
check("a new favourite replaces the old one", m.find_favorite("playlist") == "chill" and len([x for x in m.items if "playlist" in x["text"]]) == 1)

other = memory.Memory(path)                                          # a second Solu process, or the user cleaning the file
other.forget("favorite playlist")
time.sleep(0.05)
m.add("The user's sister is Mim.", "person")
check("a fact deleted elsewhere does not come back", m.find_favorite("playlist") is None)

for text, want in [("my favorite playlist on Spotify is maths", "The user's favorite playlist is maths."),
                   ("No, I said play my favorite playlist on Spotify, which is Maths.", "The user's favorite playlist is Maths."),
                   ("my friend's name is Rafi", "The user's friend's name is Rafi."),
                   ("what is my favorite playlist?", None),
                   ("my mom is a doctor", None)]:
    got = memory.from_statement(text)
    check(f"statement: {text!r}", (got[0] if got else None) == want)

print(f"\n{'all passed' if not failed else f'{failed} failed'}")
sys.exit(1 if failed else 0)
