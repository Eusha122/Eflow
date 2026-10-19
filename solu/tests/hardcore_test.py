"""Hardcore test of Solu's brain: typed requests go through the same path as speech (name, commands, skills, brain + tools).
Tools that would change the PC are RECORDED, not run. Memory, routines and tasks use temp files. Nothing is spoken.
   python tests/hardcore_test.py            all cases
   python tests/hardcore_test.py music      only cases whose group contains 'music'
"""
import argparse, os, re, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__)))); os.chdir(sys.path[0])
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
os.environ["SOLU_STT_CPU"] = "1"                                         # Eflow's own Solu has speech recognition on the card
TMP = os.path.join(tempfile.gettempdir(), "solu_hardcore")
os.makedirs(TMP, exist_ok=True)
for f in os.listdir(TMP):
    os.remove(os.path.join(TMP, f))
import memory, tasks, skills, actions
memory.PATH = os.path.join(TMP, "memory.json")
tasks.FILE = os.path.join(TMP, "tasks.json")
skills.DATA = TMP; skills.ROUTINES = os.path.join(TMP, "routines.json")
import solu_chat as s

o = s.Solu(argparse.Namespace(voice="cozy", lang="en", headphones=False, endpoint=600, mic=None, wav=None, service=True, no_screen=False))
o.memory = memory.Memory(memory.PATH)
o.in_conversation = True
o.wake_mode = True

calls, said, events = [], [], []
SAFE = {"system_info", "research", "agent_status", "clipboard_history"}   # read-only: really run


def fake(name, a):
    calls.append((name, a))
    if name in SAFE:
        return REAL(name, a)
    if name == "open_app":
        found = actions.find_app(a.get("name", ""))[0]
        return f"opened {found}" if found else f"error: no app called '{a.get('name')}' is installed (check the name)"
    if name == "play_song":
        return f"Spotify is now playing: {a.get('query')}"
    if name == "media_control":
        return {"play_pause": "pressed play/pause", "next": "skipped to the next track", "previous": "went back a track"}.get(a.get("action"), "done")
    if name == "set_volume":
        return "volume set"
    if name == "set_timer":
        return f"timer set for {a.get('seconds')} seconds"
    if name == "open_website":
        return f"opened {a.get('url')} in the browser"
    if name == "web_search":
        return f"opened a web search for '{a.get('query')}' in the browser"
    if name == "find_files":
        return "found 1: C:\\Users\\user\\Documents\\resume.pdf"
    if name == "click_element":
        return f"error: could not find '{a.get('name')}' in this window. Use look to see what is there."   # like Chrome's profile picker
    if name == "click_text":
        return f"clicked '{a.get('text')}'"
    if name == "start_task":
        return "the task was started; it runs in the background and shows on the island"
    return "done"


REAL = o.hands.run
o.hands.run = fake
o.ask_permission = lambda name, args, background=False: (events.append(("card", name, args)), "allow")[1]
o.say = lambda text: said.append(text)
o._wait_for_playback = lambda limit=45.0: None
o.respond_text = lambda text, announce=True: (said.append(text), events.append(("said", text)))
def fake_event(_event, **kw):
    events.append((_event, kw.get("text", kw)))
    if _event in ("reminder", "reminder_remove"):                       # Eflow's side: it parses and answers what to say
        calls.append((_event, kw))
        o.reminder_replies.put("Okay, I'll remind you." if _event == "reminder" else "Okay, I deleted it.")


o.event = fake_event
o.context["reminders"] = [{"title": "Gym", "when": "Daily · 7:00 AM", "note": ""}]
time.sleep(3)


def ask(text):
    """One sentence, through the same steps as Solu's listening loop (without the microphone)."""
    del calls[:]; del said[:]; del events[:]
    target = o.route(text)
    t0 = time.perf_counter()
    if target is None:
        return {"ignored": True, "text": "", "calls": [], "secs": 0}
    if target == "":
        return {"ignored": False, "text": "(name only)", "calls": [], "secs": 0}
    o.handle(target)                                                     # the same entry point the microphone loop uses
    text =" ".join(e[1] for e in events if e[0] == "said" and isinstance(e[1], str)) or " ".join(said)
    return {"ignored": False, "text": text.strip(), "calls": list(calls), "secs": time.perf_counter() - t0,
            "cards": [e[1] for e in events if e[0] == "card"]}


EMOJI = re.compile("[\U0001F000-\U0001FFFF\u2600-\u27BF]")

# (group, said, expect) - expect: tool=name | tool=None | ignored | say=regex | arg=(key, regex) | nosay=regex
CASES = [
    ("wake", "Can you hear me?", ["ignored"]),                                          # no name, not in a conversation -> set below
    ("wake", "Solu", []),
    ("chat", "Solu, how are you?", ["tool=None"]),
    ("chat", "Solu, tell me a joke", ["tool=None"]),
    ("math", "Solu, what is 12 times 8?", ["say=96|ninety-six"]),
    ("math", "Solu, what's 15 percent of 200?", ["say=30|thirty"]),
    ("time", "Solu, what time is it?", ["say=\\d"]),
    ("apps", "Solu, open Google Chrome", ["nocard", "tool=open_app", "arg=name:(?i)chrome", "result=Google Chrome"]),
    ("apps", "Solu, open Chrome", ["tool=open_app", "result=Google Chrome"]),
    ("apps", "Solu, open Spotify", ["tool=open_app", "result=Spotify"]),
    ("apps", "Solu, open calculator", ["tool=open_app", "result=Calculator"]),
    ("apps", "Solu, open file explorer", ["tool=open_app", "result=File Explorer"]),
    ("apps", "Solu, open WhatsApp", ["tool=open_app", "result=WhatsApp"]),
    ("apps", "Solu, open Discord", ["tool=open_app", "result=Discord"]),
    ("apps", "Solu, open VS Code", ["tool=open_app", "result=Visual Studio Code"]),
    ("web", "Solu, open YouTube", ["tool=open_website|open_app", "arg=*:(?i)youtube"]),
    ("web", "Solu, search Google for cheap flights to Dhaka", ["tool=web_search", "arg=query:(?i)flight"]),
    ("music", "Solu, play Shape of You by Ed Sheeran", ["tool=play_song", "arg=query:(?i)shape of you"]),
    ("music", "Solu, play Misery on Spotify", ["tool=play_song", "arg=query:(?i)misery"]),
    ("music", "Solu, play Misery on YouTube", ["tool=play_song", "arg=app:(?i)youtube"]),
    ("music", "Solu, play my maths playlist", ["tool=play_song", "arg=query:(?i)maths"]),
    ("music", "Solu, play some random music on Spotify", ["tool=play_song"]),
    ("music", "Solu, pause the music", ["tool=media_control", "arg=action:play_pause"]),
    ("music", "Solu, next song", ["tool=media_control", "arg=action:next"]),
    ("music", "Solu, put on some lo-fi", ["tool=play_song", "arg=query:(?i)lo.?fi"]),
    ("volume", "Solu, turn the volume up", ["tool=set_volume"]),
    ("volume", "Solu, mute the sound", ["tool=set_volume"]),
    ("timer", "Solu, set a timer for 5 minutes", ["tool=set_timer", "arg=seconds:^300"]),
    ("memory", "Solu, my favorite playlist on Spotify is maths", ["tool=None"]),
    ("memory", "Solu, what's my favorite playlist?", ["say=(?i)maths"]),
    ("memory", "Solu, play my favorite playlist", ["tool=play_song", "arg=query:(?i)maths"]),
    ("memory", "Solu, my name is Eusha", []),
    ("memory", "Solu, what's my name?", ["say=(?i)eusha"]),
    ("memory", "Solu, remember that my sister's birthday is on March 3rd", []),
    ("memory", "Solu, when is my sister's birthday?", ["say=(?i)march"]),
    ("memory", "Solu, what do you remember about me?", ["say=(?i)eusha|maths|march"]),
    ("context", "Solu, my friend's name is Rafi", []),
    ("context", "Solu, what's my friend's name?", ["say=(?i)rafi"]),
    ("research", "Solu, who won the last FIFA World Cup?", ["tool=research", "say=(?i)spain|(couldn.t|can.t|cannot) check"]),
    ("files", "Solu, find my resume", ["tool=find_files"]),
    ("settings", "Solu, turn off live captions", ["say=."]),
    ("settings", "Solu, turn on live captions", ["say=."]),
    ("music", "Solu, resume the music", ["tool=media_control", "arg=action:play_pause"]),
    ("music", "Solu, skip", ["tool=media_control", "arg=action:next"]),
    ("music", "Solu, go back to the previous song", ["tool=media_control", "arg=action:previous"]),
    ("music", "Solu, can you play Perfect by Ed Sheeran on YouTube", ["tool=play_song", "arg=app:youtube", "arg=query:(?i)^perfect by ed sheeran$"]),
    ("music", "Solu, play my favorite song", ["say=(?i)don't know|tell me"]),
    ("music", "Solu, play it", ["nosay=(?i)is now playing"]),
    ("memory2", "Solu, play my favorite album, my favorite playlist on Spotify, which is chill", ["tool=play_song", "arg=query:(?i)chill"]),
    ("memory2", "Solu, what's my favorite playlist?", ["say=(?i)chill"]),
    ("apps", "Solu, open my downloads folder", ["nosay=(?i)^opening downloads"]),
    ("apps", "Solu, open youtube.com", ["tool=open_website"]),
    ("research", "Solu, who is the current president of the United States?", ["tool=research", "say=(?i)trump|(couldn.t|can.t|cannot) check"]),
    ("research", "Solu, what's the price of bitcoin right now?", ["tool=research", "say=(?i)\d|(couldn.t|can.t|cannot) (check|give)|don.t (have|know)"]),
    ("chat", "Solu, I'm feeling a bit sad today", ["tool=None"]),
    ("remind", "Solu, remind me about the Codeforces contest on 4 March at 8 pm", ["tool=reminder", "arg=text:(?i)codeforces", "say=(?i)remind you"]),
    ("remind", "Solu, set an alarm for 7", ["tool=reminder", "say=(?i)remind you"]),
    ("remind", "Solu, remind me to drink water every day at 10", ["tool=reminder", "nocard"]),
    ("remind", "Solu, what are my reminders?", ["tool=None", "say=(?i)gym"]),
    ("remind", "Solu, delete the gym reminder", ["tool=reminder_remove", "arg=title:(?i)^gym$"]),
    ("click", "Solu, tap on my name", ["say=(?i)don't know your name|clicked eusha"]),
    ("click", "Solu, my name is Eusha Ibna Akbor", []),
    ("click", "Solu, what's my full name?", ["say=(?i)eusha ibna akbor"]),
    ("click", "Solu, now tap on my name", ["tool=click_element", "tool=click_text", "arg=*:^Eusha Ibna Akbor$", "say=(?i)clicked", "nocard"]),
    ("click", "Solu, click the sign in button", ["tool=click_element", "arg=name:(?i)^sign in$"]),
    ("click", "Solu, press enter", ["nosay=(?i)^clicked"]),
    ("click", "Solu, tap on Usha Ibn Akbar.", ["tool=click_text", "arg=text:^Usha Ibn Akbar$"]),          # Whisper's spelling of the name
    ("scroll", "Solu, could you please scroll down a bit?", ["nocard", "tool=scroll", "arg=direction:^down$", "arg=pages:^0.3$"]),
    ("scroll", "Solu, uh, scroll down.", ["tool=scroll", "arg=direction:^down$"]),
    ("scroll", "Solu, scroll up two pages", ["tool=scroll", "arg=direction:^up$", "arg=pages:^2$"]),
    ("scroll", "Solu, scroll to the bottom", ["tool=scroll", "arg=direction:^bottom$"]),
    ("scroll", "Solu, go to the top of the page", ["tool=scroll", "arg=direction:^top$"]),
    ("windows", "Solu, snap the window to the left", ["tool=window_control", "nocard"]),
    ("windows", "Solu, close this window", ["tool=window_control", "card=window_control"]),
    ("honest", "Solu, open Photoshop",["nosay=(?i)photoshop is (now )?open|opened photoshop"]),
]


def check(r, expect):
    bad = []
    tools = [c[0] for c in r["calls"]]
    for e in expect:
        if e == "ignored":
            if not r["ignored"]:
                bad.append("should be ignored")
        elif e.startswith("tool="):
            want = e[5:]
            if want == "None":
                if tools:
                    bad.append(f"used tools {tools}")
            elif not any(re.fullmatch(want, t) for t in tools):
                bad.append(f"no {want} call (calls: {tools})")
        elif e.startswith("arg="):
            key, rx = e[4:].split(":", 1)
            vals = [str(v) for _, a in r["calls"] for k, v in a.items() if key in ("*", k)]
            if not any(re.search(rx, v) for v in vals):
                bad.append(f"arg {key} !~ {rx} ({vals})")
        elif e.startswith("result="):
            got = [fake(n, a) for n, a in r["calls"] if n == "open_app"]
            if not any(e[7:] in g for g in got):
                bad.append(f"opened {got}")
        elif e.startswith("say="):
            if not re.search(e[4:], r["text"]):
                bad.append(f"answer !~ {e[4:]}")
        elif e == "nocard":
            if r.get("cards"):
                bad.append(f"asked first: {r['cards']}")
        elif e.startswith("card="):
            if e[5:] not in r.get("cards", []):
                bad.append(f"did not ask before {e[5:]}")
        elif e.startswith("nosay="):
            if re.search(e[6:], r["text"]):
                bad.append(f"answer claims: {e[6:]}")
    if EMOJI.search(r["text"]):
        bad.append("emoji in the answer")
    if not r["ignored"] and not r["text"]:
        bad.append("said nothing")
    if r["secs"] > 12:
        bad.append(f"slow: {r['secs']:.1f} s")
    return bad


only = sys.argv[1] if len(sys.argv) > 1 else ""
failed, total = [], 0
for group, text, expect in CASES:
    if only and only not in group:
        continue
    if group == "wake" and expect == ["ignored"]:
        o.in_conversation = False
    total += 1
    r = ask(text)
    o.in_conversation = True
    bad = check(r, expect)
    mark = "PASS" if not bad else "FAIL"
    print(f"[{mark}] {group:8} {text}\n        -> {r['text'][:160]!r}  calls={[(n, a) for n, a in r['calls']]}  {r['secs']:.1f}s")
    if bad:
        print("        !! " + "; ".join(bad))
        failed.append((text, bad))
print(f"\n{total - len(failed)}/{total} passed")
for t, b in failed:
    print(f"  FAIL {t}: {'; '.join(b)}")
os._exit(0)
