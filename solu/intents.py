"""Intents: the requests Solu handles with fixed rules instead of the language model.

A small model chats well but is unreliable with actions: it said "Media paused." without pausing anything and "Chrome is
already open" without opening it. So every request passes through this router first (Solu.handle):

    1. match() normalises the text and keeps any plain fact in it ("my favourite playlist is maths") in memory;
    2. each intent below is tried in the order it is written; the first one that recognises the request returns a handler;
    3. no intent -> the brain answers (brain.py), and may still use tools.

An intent is a function `name(solu, req) -> handler or None`, decorated with @intent. The handler takes no arguments, does
the work and speaks; it runs on a worker thread, so it may block. Adding a command = adding one function in the right
section. Keep patterns strict: a false match is worse than a miss, because a miss still reaches the brain.
"""
import queue
import re
import time
from dataclasses import dataclass
from typing import Callable, Optional

import actions
import memory as memory_mod
import research as research_mod
import tasks as tasks_mod

Handler = Callable[[], None]


@dataclass(frozen=True)
class Request:
    text: str          # what the user said to Solu (after its name)
    t: str             # the same, lower case, words only: "what's on, my screen?" -> "what's on my screen"


_INTENTS = []


def intent(fn):
    """Register an intent. Order matters: intents are tried top to bottom."""
    _INTENTS.append(fn)
    return fn


def match(solu, text) -> Optional[Handler]:
    """The handler for this request, or None when the brain should answer it."""
    req = Request(text.strip(), " ".join(re.findall(r"[a-z0-9']+", text.lower())))
    if not req.t:
        return None
    _keep_facts(solu, req)
    for fn in _INTENTS:
        handler = fn(solu, req)
        if handler is not None:
            return handler
    return None


# ---- helpers -------------------------------------------------------------------------------------------------------------------
LEAD = r"^(?:(?:can|could|would) you (?:please )?|please |hey |now |just |yeah,? |okay,? |ok,? |so,? |uh,? |um,? |and )*"   # openers before a command


def say(solu, line) -> Handler:
    return lambda: solu.respond_text(line)


def act(solu, req, tool, args, line_for) -> Handler:
    """Use one tool, then say `line_for(result)`."""
    def run():
        result = solu.use_tool(tool, args)
        print(f"   [tool] {tool}({args}) -> {result[:160]!r}")
        done(solu, req, tool, args, result, line_for(result))
    return run


def done(solu, req, tool, args, result, line):
    """Say `line` and keep the exchange in the history as a REAL tool call: plain lines ("Opening Chrome.") taught the
    brain to answer like that without calling anything."""
    solu.history += [{"role": "user", "content": req.text},
                     {"role": "assistant", "content": "", "tool_calls": [{"function": {"name": tool, "arguments": args}}]},
                     {"role": "tool", "content": result[:300], "tool_name": tool},
                     {"role": "assistant", "content": line}]
    solu.respond_text(line)


def _keep_facts(solu, req):
    """'My favourite playlist is maths', 'my sister's name is Mim': kept the moment they are said."""
    fact = memory_mod.from_statement(req.text)
    if fact and solu.memory.add(*fact):
        print(f"   [memory] {fact[0]}")


# ---- reminders and alarms (Eflow keeps them: its Reminders tab, the chime, the time parser) -----------------------------------------
NEW_REMINDER = re.compile(LEAD + r"(?:remind me\b|set (?:a |an |my )?(?:reminder|alarm)\b|(?:add|make|create) (?:a |an )?(?:reminder|alarm)\b|"
                          r"wake me(?: up)?\b|(?:a )?reminder (?:for|to|about)\b|alarm (?:at|for)\b)", re.I)
LIST_REMINDERS = re.compile(r"\b(what|which|list|show|read|tell me)\b.*\b(reminders?|alarms?)\b|\bdo i have (any )?(reminders?|alarms?)\b|"
                            r"\bwhat('s| is) (coming up|on my schedule|scheduled)\b")
REMOVE_REMINDER = re.compile(r"\b(?:delete|remove|cancel|clear|turn off)\s+(?:the |my )?(.+?)\s+(?:reminder|alarm)s?\b|"
                             r"\b(?:delete|remove|cancel|clear|turn off)\s+(?:the |my )?(?:reminder|alarm)\s+(?:for|about|called)\s+(.+)$")


@intent
def reminders(solu, req):
    """'Remind me about the Codeforces contest on 4 March at 8 pm', 'set an alarm for 7', 'what are my reminders?',
    'delete the gym reminder'. Eflow parses the time (the same parser as the island's box) and answers what to say."""
    if LIST_REMINDERS.search(req.t) and not NEW_REMINDER.match(req.text):
        items = (solu.context or {}).get("reminders")
        if items is None:
            return say(solu, "Your reminders live in Eflow's island; open the Reminders tab to see them.")
        if not items:
            return say(solu, "You don't have any reminders.")
        lines = [f"{r['title']}, {r['when'].replace(' · ', ' at ')}" for r in items[:4]]
        more = f" And {len(items) - 4} more." if len(items) > 4 else ""
        return say(solu, ("You have " + str(len(items)) + (" reminder: " if len(items) == 1 else " reminders: ") + "; ".join(lines) + "." + more))
    remove = REMOVE_REMINDER.search(req.t)
    if remove:
        return lambda: _ask_eflow(solu, "reminder_remove", title=(remove.group(1) or remove.group(2)).strip())
    if NEW_REMINDER.match(req.text):
        return lambda: _ask_eflow(solu, "reminder", text=req.text)
    return None


def _ask_eflow(solu, event, **fields):
    """Send a reminder request to Eflow and say its answer (Eflow owns the reminders and the time parser)."""
    while not solu.reminder_replies.empty():                             # an old answer that came too late
        solu.reminder_replies.get_nowait()
    solu.event(event, **fields)
    try:
        solu.respond_text(solu.reminder_replies.get(timeout=4.0))
    except queue.Empty:
        solu.respond_text("I can only set reminders when I'm running in Eflow's island.")


# ---- long tasks ----------------------------------------------------------------------------------------------------------------
@intent
def task_control(solu, req):
    """Starting, steering and answering long background tasks by voice."""
    runner, t = solu.tasks, req.t
    if runner is None:
        return None
    live = any(x.status in ("planning", "running", "waiting", "paused") for x in runner.tasks)
    if runner.waiting_question():                                             # the task asked something: this is the answer
        if re.search(r"\b(stop|cancel|abort)\b.*\btask\b", t):
            return lambda: solu.respond_text(runner.control("cancel"))
        return lambda: (runner.answer(req.text), solu.respond_text("Okay."))
    sched = tasks_mod.parse_schedule(req.text)
    if sched:
        goal, at, days = sched
        runner.add_schedule(goal, at, days)
        when = "every day" if len(days) == 7 else "on weekdays" if days == [0, 1, 2, 3, 4] else "weekly"
        return say(solu, f"Okay. {when.capitalize()} at {at} I'll {goal.rstrip('.')}.")
    if re.search(r"\b(what('s| is| are)|list|show)\b.*\b(scheduled|schedules|recurring)\b", t):
        return say(solu, "Scheduled: " + runner.list_schedules() + ".")
    m = re.match(r"^(?:please )?(?:stop|cancel|remove|delete) (?:doing |the schedule for )?(.+?) every (?:day|morning|evening|night|weekday|week)", t)
    if m:
        gone = runner.remove_schedule(m.group(1))
        return say(solu, "Okay, I won't do that every day any more." if gone else "I don't have a schedule like that.")
    if live and re.search(r"\b(pause|hold off|freeze)\b.*\b(task|work|that)\b|\bpause the task\b", t):
        return lambda: solu.respond_text(runner.control("pause"))
    if re.search(r"\b(resume|continue|carry on|keep going)\b.*\b(task|work)\b|^(continue|carry on|resume) (the )?task", t) and \
            any(x.status == "paused" for x in runner.tasks):
        return lambda: solu.respond_text(runner.control("resume"))
    if live and re.search(r"\b(stop|cancel|abort|kill|end)\b.*\b(task|that work|the work)\b|\bstop working on (it|that|this)\b", t):
        return lambda: solu.respond_text(runner.control("cancel"))
    if re.search(r"\b(clear|remove|delete|forget)\b.*\b(finished|done|old|completed) tasks?\b", t):
        return lambda: ([runner.control("remove", x.id) for x in list(runner.tasks) if x.status in ("done", "failed", "cancelled")],
                        solu.respond_text("Cleared the finished tasks."))
    if re.search(r"\b(list|show|what)\b.*\btasks\b", t):
        return lambda: solu.respond_text(runner.list_text() + ".")
    if runner.tasks and re.search(r"\btask\b.*\b(status|going|progress|doing|done yet)\b|\bhow('s| is) the task\b|"
                                  r"\bwhat are you (working on|doing)\b|\bany progress\b", t):
        return lambda: solu.respond_text(runner.status_text())
    m = re.match(r"^(?:please )?(?:start|begin|run|create|set up|give you|do)(?: a| the| this)?(?: long| big| background)? task[:,]?\s+(.{8,})$", req.text, re.I) or \
        re.match(r"^(?:your )?(?:task|goal) (?:is|:)\s*(.{8,})$", req.text, re.I) or \
        re.match(r"^(?:i want you to|i need you to|can you|could you) (?:go and |now )?(?:work on|take care of|figure out|handle) "
                 r"(.{8,}?)(?: (?:by yourself|on your own|in the background))?$", req.text, re.I)
    if m:
        goal = m.group(1).strip().rstrip(".")
        return lambda: (runner.submit(goal), solu.respond_text("Okay, I'm on it. You can follow it on the island."))
    return None


# ---- the study quiz (while one runs, everything said is an answer) -------------------------------------------------------------
@intent
def quiz(solu, req):
    if not solu.quiz:
        return None
    if re.search(r"\b(stop|end|quit|finish|cancel)\b.*\bquiz\b|\b(stop|enough|that's enough)\b$", req.t):
        return solu.end_quiz
    return lambda: solu.quiz_answer(req.text)


# ---- music and media ---------------------------------------------------------------------------------------------------------------
MEDIA = re.compile(LEAD + r"(?:(pause|stop|resume|unpause|continue)(?: (?:the|my|this))? (?:music|song|track|video|spotify|playback|it)|"
                   r"(next|skip)(?: (?:the|this))?(?: (?:song|track|one|video))?|(previous|last|go back(?: to the)?)(?: (?:song|track|one|video))|"
                   r"play (?:the )?(previous|last) (?:song|track|one))[.!?]*$", re.I)


@intent
def media_keys(solu, req):
    """Pause / resume / next / previous: the media keys."""
    m = MEDIA.match(req.text)
    if not m:
        return None
    action = "play_pause" if m.group(1) else "next" if m.group(2) else "previous"
    done = {"play_pause": "Okay." if re.match(r"(?i)(resume|unpause|continue)", m.group(1) or "") else "Paused.",
            "next": "Next one.", "previous": "Going back."}[action]
    return act(solu, req, "media_control", {"action": action},
               lambda r: done if not r.startswith("error") else "Sorry, I couldn't control the music.")


PLAY = re.compile(LEAD + r"(?:play|put on|start playing|listen to|i want to (?:hear|listen to)|i wanna (?:hear|listen to))\s+(.+?)(?: please)?[.!?]*$", re.I)
GENERIC_PLAY = re.compile(r"^(it|that|this|music|the music|some music|something|a song|songs?|again|it again|the song|my music|"
                          r"(the )?next (one|song)|anything)$", re.I)
FAVOURITE = re.compile(r"\b(?:my )?(?:favou?rite|usual)\s+(playlist|song|album|artist|band|music)\b", re.I)


@intent
def play_music(solu, req):
    """'Play Shape of You', 'put on my maths playlist', 'play my favourite playlist on YouTube'."""
    m = PLAY.match(req.text)
    if not m:
        return None
    what = m.group(1).strip(" ,.")
    app = "youtube" if re.search(r"\b(on|in|from) youtube\b", what, re.I) else "spotify"
    what = re.sub(r"\s*\b(on|in|from|with) (spotify|youtube)\b", "", what, flags=re.I).strip(" ,.")
    what = re.sub(r"^(me |for me |us )|\s+for me$", "", what, flags=re.I).strip()
    if not what or GENERIC_PLAY.match(what):
        return None                                                           # "play it" / "play music": resuming is the brain's call
    favourites = list(FAVOURITE.finditer(what))                               # "my favourite album, my favourite playlist, which is chill"
    if favourites:
        known = next((k for k in (solu.memory.find_favorite(f.group(1)) for f in reversed(favourites)) if k), None)
        if not known:
            kind = favourites[-1].group(1)
            return say(solu, f"I don't know your favorite {kind} yet. Tell me, and I'll remember it.")
        what = known

    def line(result):
        playing = re.match(r"(?:Spotify is now playing|opened '.+' on YouTube)[: ]*(.*)", result)
        if playing:
            name = re.sub(r"\s*\(from the user's library\)", "", playing.group(1) or what).strip(" .'") or what
            return f"Playing {name} on YouTube." if app == "youtube" else f"Playing {name}."
        if result.startswith("opened the search"):
            return f"I opened {what} in Spotify, but couldn't press play. Pick it there."
        return f"Sorry, I couldn't play {what}."
    return act(solu, req, "play_song", {"query": what, "app": app}, line)


# ---- the screen and apps -----------------------------------------------------------------------------------------------------------
CLICK = re.compile(LEAD + r"(?:tap|click|press|select|choose|hit|pick)(?: on| the)* (.+?)(?: button| option| profile| link| icon| tab)?(?: please)?[.!?]*$", re.I)
KEY = re.compile(r"(?i)(the )?(enter|return|escape|esc|tab|space|spacebar|backspace|delete|del|all|ctrl.*|control.*|alt.*|shift.*|"
                 r"windows.*|f\d+|up|down|left|right|page up|page down|home|end)( key)?")
ME = re.compile(r"(?i)(on )?(me|my name|my profile|my account|myself|mine|my picture|my photo)")


@intent
def click(solu, req):
    """'Tap on my name', 'click the sign in button'. Windows' accessibility tree first, then the pixels (OCR): Chrome's
    profile picker, for one, exposes nothing to UI Automation."""
    m = CLICK.match(req.text)
    if not m or KEY.fullmatch(m.group(1).strip(" .")):                       # "press enter" is a key, not something to click
        return None
    what = m.group(1).strip(" ,.")
    if ME.fullmatch(what):
        what = solu.memory.user_name()
        if not what:
            return say(solu, "I don't know your name yet. Say: my name is, and then your name.")

    def run():
        result = solu.use_tool("click_element", {"name": what})
        if result.startswith("error"):
            result = solu.use_tool("click_text", {"text": what})
        print(f"   [tool] click({what!r}) -> {result!r}")
        if result.startswith(("clicked", "double-clicked", "right-clicked")):
            line = f"Clicked {what}."
        else:
            line = "Okay, I won't." if result.startswith("the user said no") else f"I couldn't find {what} on the screen."
        done(solu, req, "click_element", {"name": what}, result, line)
    return run


SCROLL = re.compile(LEAD + r"(?:scroll|go|move|page)\s+(up|down)(?:\s+(a bit|a little(?: bit)?|slightly|more|a lot|a page|one page|"
                    r"(?:two|three|\d+) pages|to the (?:top|bottom)))?(?: please)?[.!?]*$|"
                    + LEAD + r"(?:scroll |go )?to the (top|bottom)(?: of (?:the|this) page)?(?: please)?[.!?]*$", re.I)


@intent
def scroll(solu, req):
    """'Scroll down a bit', 'scroll up', 'scroll to the bottom': the window the user is in."""
    m = SCROLL.match(req.text)
    if not m:
        return None
    amount = (m.group(2) or "").lower()
    if m.group(3) or "top" in amount or "bottom" in amount:
        direction, pages = (m.group(3) or ("top" if "top" in amount else "bottom")).lower(), 1
    else:
        direction = m.group(1).lower()
        words = {"two": 2, "three": 3}
        many = re.match(r"(two|three|\d+) pages", amount)
        pages = (words.get(many.group(1)) or int(many.group(1))) if many else \
            0.3 if amount in ("a bit", "a little", "a little bit", "slightly", "") else 0.6 if amount == "more" else 1 if "page" in amount else 2
    return act(solu, req, "scroll", {"direction": direction, "pages": pages},
               lambda r: "Okay." if not r.startswith("error") else "There's nothing I can scroll right now.")


OPEN = re.compile(LEAD + r"(?:open|launch|start|run)(?: up)? (?:the |my )?(.+?)(?: app| application| for me)?(?: please)?[.!?]*$", re.I)
NOT_AN_APP = re.compile(r"\.(com|org|net|io)\b|\b(website|site|page|file|folder|document|tab)\b|youtube|gmail|chatgpt|facebook|instagram", re.I)


@intent
def open_app(solu, req):
    """'Open Chrome': an installed app, opened for certain. Websites, files and folders are left to the brain."""
    m = OPEN.match(req.text)
    if not m or NOT_AN_APP.search(m.group(1)):
        return None
    found = actions.find_app(m.group(1))[0]
    if not found:
        return None
    return act(solu, req, "open_app", {"name": m.group(1)},
               lambda r: f"Opening {found}." if r.startswith("opened") else f"Sorry, I couldn't open {m.group(1)}.")


# ---- notes, routines, briefing, quick capture --------------------------------------------------------------------------------------
@intent
def notes(solu, req):
    t = req.t
    if re.search(r"\b(stop|end|finish|done|that's it)\b.*\b(taking )?(notes?|recording|transcri\w*)\b|\bstop (the )?(lecture|meeting)\b|"
                 r"\bnotes? (are )?(done|finished)\b", t):
        return lambda: solu.respond_text(solu._notes_stop_line())
    start = re.search(r"\b(?:start|begin|take|record|make|write)\b.*\b(?:notes?|transcri\w*)\b", t) or \
        re.fullmatch(r"(lecture|meeting|class|video) notes?( please)?", t)
    if start and not solu.notes and re.search(r"\b(lecture|meeting|video|class|call|this|notes?)\b", t) and \
            not re.search(r"\b(stop|end|my notes|open|read)\b", t):
        kind = "meeting" if re.search(r"\b(meeting|call|zoom|meet)\b", t) else "video" if "video" in t else "lecture"
        return lambda: solu.respond_text(solu._notes_start_line(kind))
    return None


@intent
def routines(solu, req):
    """'When I say study mode, open my notes and play lo-fi' / 'study mode' / 'list my routines'."""
    defined = solu.routines.define(req.text)
    if defined:
        name, steps = defined
        mine = steps.rstrip(".")
        for a, b in ((r"\bme\b", "you"), (r"\bmy\b", "your"), (r"\bI\b", "you"), (r"\bmine\b", "yours")):   # "tell me" -> "tell you"
            mine = re.sub(a, b, mine)
        return say(solu, f"Okay. Whenever you say {name}, I'll {mine}.")
    name = solu.routines.match(req.text)
    if name:
        return lambda: solu.run_routine(name)
    if re.search(r"\b(list|what are) (my |the )?routines\b", req.t):
        names = list(solu.routines.items)
        return say(solu, ("Your routines are: " + ", ".join(names) + ".") if names else "You don't have any routines yet.")
    m = re.search(r"\b(?:delete|remove|forget) (?:the |my )?routine (?:called )?(.+)$", req.t)
    if m:
        gone = solu.routines.delete(m.group(1))
        return say(solu, f"Deleted the {gone} routine." if gone else "I don't have a routine like that.")
    return None


@intent
def briefing(solu, req):
    if re.search(r"^(good morning|morning( solu)?|morning briefing|brief me|give me (my|a) briefing|what('s| is) (my day|on today|up today))\b", req.t):
        return solu.briefing
    return None


@intent
def quick_capture(solu, req):
    """'Let me show you' / 'translate this part': the user drags over an area of the screen."""
    if re.search(r"\b(let me show you|i'll show you|i will show you|look at this (part|area|bit)|translate this (part|area|bit))\b", req.t):
        mode = "translate" if "translat" in req.t else "explain"
        return lambda: (solu.event("capture", mode=mode), solu.respond_text("Okay, drag over the part you mean."))
    return None


@intent
def screen_question(solu, req):
    """'What does this error mean?', 'look at this'."""
    if solu.is_screen_request(req.text):
        return lambda: solu.respond_screen(req.text)
    return None


# ---- settings ----------------------------------------------------------------------------------------------------------------------
def _switch(t, off_words, on_words):
    """True / False when the sentence switches something on / off, None when it does neither."""
    off, on = re.search(off_words, t), re.search(on_words, t)
    return None if not (off or on) else not off


@intent
def captions(solu, req):
    if not re.search(r"\bcaptions?\b", req.t):
        return None
    value = _switch(req.t, r"\b(off|disable|hide|stop|without)\b", r"\b(on|enable|show|start|with)\b")
    if value is None:
        return None
    return lambda: (solu.event("setting", name="captions", value=value), solu.respond_text(f"Live captions are {'on' if value else 'off'}."))


@intent
def headphones(solu, req):
    t = req.t
    if not (re.search(r"\bheadphones?\b", t) or re.search(r"\b(using|on|switch to|back to)\s+(the\s+)?speakers?\b", t)):
        return None
    off = re.search(r"\b(off|took|taken|removed|without|disable|stop)\b", t) or re.search(r"\bspeakers?\b", t)
    on = re.search(r"\b(on|wearing|put|using|mode|enable|got)\b", t)
    if not (off or on):
        return None
    value = not off

    def run():
        solu.half_duplex = not value
        solu.event("setting", name="headphones", value=value)
        solu.respond_text("Headphones mode is on. You can talk over me." if value else "Speaker mode. I'll wait until I'm done talking.")
    return run


@intent
def voice(solu, req):
    m = re.search(r"\b(cozy|cosy|bubbly|tiny|calm|natural)\b", req.t)
    if not m or not re.search(r"\b(voice|use|switch|change|set|sound)\b", req.t):
        return None
    name = "cozy" if m.group(1) == "cosy" else m.group(1)

    def run():
        solu.args.voice = name
        solu.event("setting", name="voice", value=name)
        solu.respond_text(f"Okay, this is my {name} voice.")
    return run


@intent
def sleep(solu, req):
    if not re.search(r"\b(go to sleep|stop listening|turn yourself off|switch yourself off|sleep now|shut down)\b", req.t):
        return None

    def run():
        solu._after = lambda: solu.event("setting", name="enabled", value=False)
        solu.respond_text("Okay, going to sleep. Switch me back on from the island.")
    return run


@intent
def screen_help_setting(solu, req):
    if not re.search(r"\bscreen help\b", req.t):
        return None
    value = _switch(req.t, r"\b(off|disable|stop|without)\b", r"\b(on|enable|start|with)\b")
    if value is None:
        return None

    def run():
        solu.screen_enabled = value
        solu.event("setting", name="screen", value=value)
        solu.respond_text(f"Screen help is {'on' if value else 'off'}.")
    return run


# ---- research reports --------------------------------------------------------------------------------------------------------------
@intent
def research_reports(solu, req):
    """'How's the research going?', 'read me the summary', 'read the whole report', 'open my last report'."""
    t = req.t
    if re.search(r"\bhow('s| is) (the |my |that )?(research|report)( going)?\b|\b(is|are) (the |my )?(research|report) (done|ready|finished)\b", t):
        running = [j for j in solu.jobs if j.is_alive()]
        if running:
            j = running[0]
            return say(solu, f"Still working on {j.title}. I'm {j.status} right now, about {int(100 * j.progress)} percent done.")
        title, _, _ = solu.latest_job()
        return say(solu, f"Nothing is running. Your last report was {title}." if title else "There's no research going on right now.")
    recent = any(j.status == "done" and time.time() - j.started < 3600 for j in solu.jobs)
    whole = re.search(r"\bread (me )?(the |that )?(whole|full|entire|complete) (report|research|doc|document|thing)\b", t)
    summary = re.search(r"\bread (me )?(the |that )?(summary|report|research)\b|\bwhat did (you|the research|the report) (find|say)\b", t) or \
        (recent and re.fullmatch(r"(yes|yeah|sure|ok|okay|please)?\s*(please )?(read it|read that|read it to me|yes read it|go ahead)( please)?", t))
    if whole or summary:
        title, markdown, _ = solu.latest_job()
        if not markdown:
            return say(solu, "I don't have a research report yet. Ask me to do deep research on something.")
        if whole:
            return say(solu, solu.for_reading(markdown))
        part = solu.report_section(markdown, "Summary") or solu.for_reading(markdown)[:900]
        return say(solu, f"Here's the summary of {title}. " + solu.for_reading(part))
    if re.search(r"\bopen (the |my |that )?(last |latest |previous )?(report|research)\b", t):
        _, _, path = solu.latest_job()
        if not path:
            return say(solu, "I don't have a research report yet.")
        return lambda: (research_mod.open_report(path), solu.respond_text("Opened it in Chrome."))
    return None


# ---- memory --------------------------------------------------------------------------------------------------------------------------
@intent
def memory_commands(solu, req):
    """'What do you remember about me?', 'forget that ...', 'remember that ...', 'forget everything'."""
    t = req.t
    if re.search(r"\b(forget|erase|delete|clear|wipe)\b.*\b(everything|all of it|all about me|your memory|all memories|all your memories)\b", t):
        return lambda: (solu.memory.clear(), solu.respond_text("Okay, I've forgotten everything about you. It's a fresh start."))
    if re.search(r"\bwhat (do|did) you (remember|know) about me\b|\bwhat do you remember\b|\bwhat('s| is) in your memory\b", t):
        facts, total = solu.memory.summary(5)
        if not facts:
            return say(solu, "I don't remember anything about you yet. Tell me something, or say remember that, and I'll keep it.")
        more = f" And {total - 5} more things." if total > 5 else ""
        return say(solu, "Here's what I remember: " + " ".join(memory_mod.spoken(f) for f in facts) + more)
    m = re.match(r"^(?:please |can you |could you )?forget (?:that |about |the fact that )?(.+)$", t)
    if m and not re.search(r"\b(allowed|permission)", t):
        gone = solu.memory.forget(m.group(1))
        if not gone:
            return say(solu, "I don't have anything like that in my memory.")
        told = memory_mod.spoken(gone)
        return say(solu, f"Okay, I've forgotten that {told[0].lower()}{told[1:]}")
    m = re.search(r"(?:^|\b(?:please|can you|could you|and)\s+)(?:remember|don't forget|keep in mind|note)\s+(?:that\s+)?(.{6,})$", t)
    if m and not re.search(r"\b(do|did|can't|cant|don't|dont) (you|i) remember\b|\bremember when\b", t):
        said = re.search(r"(?:remember|don't forget|keep in mind|note)\s+(?:that\s+)?(.{6,})$", req.text, re.I)
        fact = (said.group(1) if said else m.group(1)).strip().rstrip(".!")
        return lambda: (solu.memory.add(f"The user told Solu: {fact}.", "fact", "told"), solu.respond_text("Got it, I'll remember that."))
    return None


# ---- conversation control ------------------------------------------------------------------------------------------------------------
@intent
def stop_talking(solu, req):
    if req.t not in {"stop", "be quiet", "quiet", "shush", "enough", "stop talking", "okay stop", "hush"}:
        return None

    def run():
        solu.cancel.set()
        while True:
            try:
                solu.audio_out.get_nowait()
            except queue.Empty:
                break
        solu.event("state", state="listening" if solu.in_conversation else "ready")
    return run


@intent
def goodbye(solu, req):
    if re.match(r"^(bye|goodbye|that's all|thats all|thanks bye|see you|talk later|never mind|nevermind)", req.t):
        def run():
            solu._after = solu.end_conversation
            solu.respond_text("Okay, bye!")
        return run
    return None
