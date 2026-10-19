"""Solu's task runner: long goals done by themselves, in the background, without getting in the conversation's way.

A task is a goal, a short plan of checkable steps, a scratchpad of notes and a log. One worker thread runs one task at a time:
for every step it looks at the screen, decides ONE action, does it, and checks the result before moving on; a step that does not
work is retried differently, then the plan is revised, and if it is still stuck Solu asks the user. Tasks survive restarts (saved
to %LOCALAPPDATA%\\Eflow\\solu_tasks.json), can be paused / stopped / answered by voice, and can repeat on a schedule.

Cheap on purpose: prompts are small and share the same start (the model's cache is reused), screenshots are only read when a GUI
step needs them, and every model call waits while Solu or the user is talking and is dropped the moment the user starts to speak.
"""
import ctypes
import datetime
import hashlib
import json
import os
import re
import threading
import time
import urllib.request
import uuid

import actions
import gpu

DATA = os.path.join(os.environ.get("LOCALAPPDATA", "."), "Eflow")
FILE = os.path.join(DATA, "solu_tasks.json")
# the user's real folders, so "my Documents" is never guessed (the model once wrote to C:\Users\Public\Documents)
FOLDERS = "USER FOLDERS: " + ", ".join(f"{n} = {os.path.join(actions.HOME, n)}" for n in ("Documents", "Desktop", "Downloads"))

MAX_STEP_ACTIONS = 12                       # actions on one step before it counts as failed
MAX_BURST = 4                               # actions the model may send in one turn (they run back to back)
SETTLE = 0.2                                # after the last action of a turn: let the window draw before looking
SETTLE_BETWEEN = 0.12                       # between actions of one burst
CONTROL_TOOLS = {"step_done", "give_up", "ask_user", "note"}       # turns about the task itself, not actions on the PC
MAX_ACTIONS = 90                            # per task, until the user says to carry on
MAX_MINUTES = 120
KEEP_FINISHED = 12
ANSWER_WINDOW = 300                         # seconds after a task's question in which what the user says is taken as the answer
GUI_ACTIONS = {"click_element", "click_text", "type_text", "press_keys", "scroll", "drag"}      # these act in whatever window is in front
GUI_TOOLS = {"click_element", "click_text", "type_text", "press_keys", "scroll", "drag", "window_control", "open_app", "open_website",
             "open_file", "wait_for_text"}
GUI_WORDS = re.compile(r"\b(click|open|type|press|tab|window|button|menu|page|screen|drag|select|field|scroll|tick|check|enter|fill|"
                       r"log ?in|sign|chrome|browser|notepad|settings|app|folder|explorer|download|upload|search box)\b", re.I)

PLAN = ("Plan how to reach this goal on the user's Windows PC.\nGOAL: {goal}\n\n"
        "You can: open apps and websites; look at the screen and click, type, press keys, scroll and drag in any window; "
        "search, read and write files; run PowerShell; research the web; write email or messages (the user approves each); "
        "hand coding or building work to the user's coding agents (Claude Code / Codex) and wait for them; wait for things to load.\n"
        "Write 3 to 9 concrete steps, each something that can be checked afterwards. Use as few steps as possible. "
        "Coding, building or fixing software is ONE step that delegates to an agent and waits for it. Don't plan steps just to 'review' or 'verify' "
        "what an earlier step already proves. To create or change a text file, write it directly (one step): never open Notepad or another "
        "editor for that. Never close, move or type into windows the goal does not mention. Don't plan a step to open a window the goal says is already there. To find facts on the web use the research tool "
        "(it searches and reads pages by itself): never plan to open a browser for that. Don't plan a step to tell or notify the user: "
        "the result is told to them automatically when the task ends.\n"
        'Return JSON: {{"title": "3-6 words", "steps": [{{"text": "what to do", "done_when": "how to tell it worked"}}], '
        '"needs": ["which of: apps, typing, files, web, commands, agents, messages, email"]}}')
REPLAN = ("A step of a task did not work. Revise the REMAINING plan.\nGOAL: {goal}\nDONE SO FAR: {done}\nFAILED STEP: {step}\n"
          "WHAT HAPPENED: {why}\nNOTES: {notes}\n"
          "Write the remaining steps (2 to 8) taking another approach to the failed part. Keep steps COARSE (one step may involve many actions); "
          "never split a step into many tiny steps and never add steps that only wait. Same JSON as before: "
          '{{"steps": [{{"text": "...", "done_when": "..."}}]}}')
SYSTEM = ("You are Solu's task worker. You operate the user's Windows PC to reach a goal, one step at a time.\n"
          "Each turn call ONE tool, or, when you are sure of a short sequence on the screen you see (click a field, type, press Enter), up to "
          "4 actions in a row: they run at once and you see the screen after the last. Never put step_done in the same turn as actions.\n"
          "Rules: use look to see the screen before clicking if you are not sure what is there. Click by the control's name (click_element). "
          "Work ONLY on the CURRENT step: never do a later step's actions early. To type into a field, use type_text with 'into' set to the field's name or label: it clicks the field first. "
          "After an action read its result and the screen shown with it. When the CURRENT step's goal is met, call step_done with what you saw "
          "as proof. If you need something only the user knows or must do (a login, a password, a choice), call ask_user. If it truly cannot be "
          "done, call give_up. Never claim something worked without seeing it. If an action changed nothing, do something different. "
          "Keep notes (the note tool) for facts you need later, like a file path or an address. "
          "A step that only tells, informs or reports to the user: call step_done at once with what to tell them (it is said when the task ends). "
          "After each action you are shown what CHANGED on the screen. If that change shows the current step is achieved (a counter moved, a box "
          "shows ON, text appeared), call step_done at once: do NOT repeat the action. If nothing changed, the action did not work: do something else.")
VERIFY = ("Check one step of a task.\nSTEP: {step}\nDONE WHEN: {done_when}\nTHE WORKER SAYS: {claim}\nWHAT IS ON THE SCREEN NOW:\n{screen}\n"
          'Answer done=true when the screen shows the DONE WHEN result (or the step has no visible result, like reading or deciding). '
          'Answer done=false when the screen shows an error, contradicts the claim, or shows no sign of a result that should be visible '
          '(for example the worker says it moved or typed something but the screen does not show it). '
          'JSON: {{"done": true or false, "why": "one short sentence"}}')
SUMMARY = ("The user asked Solu to: {goal}\nSteps and results:\n{steps}\n"
           "Write what Solu should say out loud now: one or two short, warm sentences saying what was achieved (and anything that was not). "
           "Plain words, no lists.")

TOOLS = [
    actions._tool("step_done", "The current step is complete. Say what you saw that proves it.", {"proof": actions._s("what shows it worked")}, ["proof"]),
    actions._tool("give_up", "The current step cannot be done. Say why.", {"reason": actions._s("why it is impossible")}, ["reason"]),
    actions._tool("ask_user", "Ask the user a question (or for them to do something, like log in) and wait for the answer.",
                  {"question": actions._s("one short spoken question")}, ["question"]),
    actions._tool("note", "Write a fact in the task's notes so it is remembered for later steps.",
                  {"key": actions._s("short name"), "value": actions._s("the fact")}, ["key", "value"]),
]
BASE_TOOLS = ["look", "click_element", "type_text", "press_keys", "scroll", "wait", "wait_for_text", "window_control", "open_app", "open_website"]


class Cancelled(Exception):
    pass


class Parked(Exception):
    """The task asked the user something and waits: the worker is free for other tasks until the answer comes."""


class Yield(Exception):
    """The user started talking: this model call was dropped to leave the model free for them."""


def _json(text):
    """The first {...} object in a model's answer (models sometimes wrap it in words)."""
    text = (text or "").strip()
    try:
        return json.loads(text)
    except ValueError:
        pass
    a, b = text.find("{"), text.rfind("}")
    if a >= 0 and b > a:
        try:
            return json.loads(text[a:b + 1])
        except ValueError:
            pass
    return {}


def short(text, n=160):
    text = " ".join(str(text).split())
    return text if len(text) <= n else text[:n - 1] + "…"


class Task:
    def __init__(self, goal, schedule_id=None):
        self.id = uuid.uuid4().hex[:8]
        self.goal = goal.strip()
        self.title = short(goal, 40)
        self.steps = []                                   # {"text", "done_when", "status": todo|doing|done|failed|skipped, "result", "tries"}
        self.status = "planning"                          # planning running paused waiting done failed cancelled
        self.created = time.time()
        self.finished = 0.0
        self.notes = {}
        self.log = []                                     # {"t", "text"}: what was done, newest last
        self.actions = 0
        self.budget = MAX_ACTIONS
        self.deadline = time.time() + MAX_MINUTES * 60
        self.question = ""
        self.result = ""
        self.schedule_id = schedule_id
        self.replans = 0
        self.needs = []
        self.hwnd = 0                                      # the window the last action left in front: actions only go there
        self.own = []                                      # windows this task opened itself (it may close or move only these, or ones named in the goal)
        self.error = ""
        self.pending = None                                # {"kind": ask|stuck|budget, "question", "step"}: a question that is waiting for the user
        self.reply = None                                  # the user's answer, applied when the task resumes
        self.hint = ""                                     # what to tell the model on its next turn

    def to_dict(self):
        return dict(self.__dict__)

    @classmethod
    def from_dict(cls, d):
        t = cls(d.get("goal", ""))
        t.__dict__.update(d)
        return t

    def add_log(self, text):
        self.log.append({"t": time.time(), "text": short(text, 300)})
        if len(self.log) > 120:
            self.log = self.log[-120:]

    @property
    def done_count(self):
        return sum(1 for s in self.steps if s["status"] in ("done", "skipped"))

    def current(self):
        return next((s for s in self.steps if s["status"] in ("todo", "doing")), None)


class TaskRunner:
    def __init__(self, solu, ollama, model, ctx):
        self.solu, self.ollama, self.model, self.ctx = solu, ollama, model, ctx
        self.lock = threading.RLock()
        self.cond = threading.Condition(self.lock)
        self.tasks, self.schedules = [], []
        self._typed = {}
        self.stopping = False
        self.current = None
        self.tls = threading.local()
        self._last_emit = 0.0
        self.load()
        threading.Thread(target=self._main, daemon=True, name="solu-tasks").start()

    # ---- saving -------------------------------------------------------------------------------------------------------
    def load(self):
        try:
            with open(FILE, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return
        self.tasks = [Task.from_dict(d) for d in data.get("tasks", [])]
        self.schedules = data.get("schedules", [])
        for t in self.tasks:
            if t.status in ("running", "planning", "waiting"):           # the app was closed in the middle of it
                t.status = "paused"
                t.question, t.pending, t.reply = "", None, None
                t.add_log("interrupted: Solu was closed; say 'continue the task' to carry on")

    def save(self):
        try:
            os.makedirs(DATA, exist_ok=True)
            keep = [t for t in self.tasks if t.status not in ("done", "failed", "cancelled")] + \
                   sorted([t for t in self.tasks if t.status in ("done", "failed", "cancelled")], key=lambda t: t.finished)[-KEEP_FINISHED:]
            self.tasks = sorted(keep, key=lambda t: t.created)
            tmp = FILE + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"tasks": [t.to_dict() for t in self.tasks], "schedules": self.schedules}, f, ensure_ascii=False, indent=1)
            os.replace(tmp, FILE)
        except OSError:
            pass

    # ---- what the app and the user see --------------------------------------------------------------------------------
    def snapshot(self):
        with self.lock:
            out = []
            for t in self.tasks[-8:][::-1]:
                step = t.current()
                out.append({"id": t.id, "title": t.title, "status": t.status, "done": t.done_count, "total": len(t.steps),
                            "step": short(step["text"], 80) if step else ("Finishing up" if t.status == "running" else ""), "question": t.question, "result": short(t.result, 160)})
            return out

    def emit(self, force=False):
        """Tell the island: the task list, and a live pill for the one that is running."""
        now = time.monotonic()
        if not force and now - self._last_emit < 0.4:
            return
        self._last_emit = now
        snap = self.snapshot()
        try:
            self.solu.event("tasks", list=snap)
            live = next((t for t in snap if t["status"] in ("running", "planning", "waiting", "paused")), None)
            if live:
                detail = live["question"] if live["status"] == "waiting" else live["step"] or live["title"]
                title = {"waiting": "Needs you", "paused": "Task paused", "planning": "Planning"}.get(live["status"], "Task")
                self.solu.event("activity", id="task", title=title, detail=short(f"{live['title']} · {detail}", 60),
                                progress=round(live["done"] / live["total"], 2) if live["total"] else -1)
            else:
                self.solu.event("activity_end", id="task")
        except Exception:                                                          # noqa: BLE001
            pass

    def status_text(self):
        with self.lock:
            t = (next((x for x in reversed(self.tasks) if x.status == "waiting" and x.pending), None) or self.current
                 or next((x for x in reversed(self.tasks) if x.status in ("running", "waiting", "paused", "planning")), None))
            if not t:
                last = self.tasks[-1] if self.tasks else None
                return f"No task is running. The last one, {last.title}, {last.status}." if last else "I have no tasks right now."
            step = t.current()
            if t.status == "waiting":
                if t.pending:
                    t.pending["asked"] = time.time()                                     # asked again: the next thing said answers it
                return f"I'm waiting for you on {t.title}: {t.question}"
            if t.status == "paused":
                return f"{t.title} is paused at step {t.done_count + 1} of {len(t.steps)}."
            if t.status == "planning":
                return f"I'm still planning {t.title}."
            return (f"Working on {t.title}: step {t.done_count + 1} of {len(t.steps)}, "
                    f"{step['text'] if step else 'finishing up'}. That's {t.actions} actions so far.")

    def list_text(self):
        with self.lock:
            if not self.tasks:
                return "I have no tasks."
            return "; ".join(f"{t.title} ({t.status}{', ' + str(t.done_count) + ' of ' + str(len(t.steps)) if t.steps and t.status not in ('done',) else ''})"
                             for t in self.tasks[-5:])

    def waiting_question(self, fresh=ANSWER_WINDOW):
        """The question a task is waiting on, if it was asked (or repeated) recently: only then is what the user says its answer."""
        with self.lock:
            t = next((x for x in reversed(self.tasks) if x.status == "waiting" and x.pending), None)
            if not t or time.time() - t.pending.get("asked", 0) > fresh:
                return ""
            return t.pending["question"]

    # ---- control ------------------------------------------------------------------------------------------------------
    def submit(self, goal, schedule_id=None):
        t = Task(goal, schedule_id)
        with self.cond:
            self.tasks.append(t)
            t.add_log("started")
            self.save()
            self.cond.notify_all()
        self.emit(True)
        return t

    def _find(self, ident=None):
        if ident:
            return next((t for t in self.tasks if t.id == ident), None)
        live = [t for t in self.tasks if t.status in ("running", "planning", "waiting", "paused")]
        return live[-1] if live else (self.tasks[-1] if self.tasks else None)

    def control(self, op, ident=None):
        with self.cond:
            t = self._find(ident)
            if not t:
                return "There's no task."
            if op == "pause" and t.status in ("running", "planning", "waiting"):
                t.status, t.question, t.pending, t.reply = "paused", "", None, None   # resuming retries the step rather than re-asking
                t.add_log("paused by the user")
                text = f"Paused {t.title}."
            elif op == "resume" and t.status in ("paused",):
                t.status = "running" if t.steps else "planning"
                t.budget = max(t.budget, t.actions + MAX_ACTIONS // 2)
                t.deadline = max(t.deadline, time.time() + 30 * 60)
                t.add_log("resumed by the user")
                text = f"Carrying on with {t.title}."
            elif op == "cancel" and t.status not in ("done", "failed", "cancelled"):
                t.status, t.question, t.finished = "cancelled", "", time.time()
                t.add_log("stopped by the user")
                t.pending = None
                text = f"Stopped {t.title}."
            elif op == "remove":
                self.tasks = [x for x in self.tasks if x is not t] if t.status in ("done", "failed", "cancelled") else self.tasks
                text = "Removed."
            else:
                text = f"{t.title} is {t.status}."
            self.save()
            self.cond.notify_all()
        self.emit(True)
        return text

    def answer(self, text):
        """The user's answer to a question a task asked: the task carries on (it resumes when the worker is free)."""
        with self.cond:
            t = next((x for x in reversed(self.tasks) if x.status == "waiting" and x.pending), None)
            if t:
                t.reply, t.status, t.question = text, "running", ""
                t.add_log(f"you said: {short(text, 80)}")
                self.save()
                self.cond.notify_all()
        self.emit(True)

    def cancelled(self):
        """True when the running task was stopped or paused: long waits give up."""
        t = self.current
        return bool(t and t.status in ("cancelled", "paused"))

    # ---- the worker ---------------------------------------------------------------------------------------------------
    def _main(self):
        last_schedule = 0.0
        while not self.stopping:
            with self.cond:
                task = next((t for t in self.tasks if t.status in ("planning", "running")), None)
                if task is None:
                    self.cond.wait(timeout=15)
            if time.monotonic() - last_schedule > 20:
                last_schedule = time.monotonic()
                self._check_schedules()
            if task is None:
                continue
            self.current = task
            self.tls.task = task
            try:
                self._run(task)
            except Cancelled:
                pass
            except Parked:
                pass
            except Yield:
                pass
            except Exception as e:                                                  # noqa: BLE001
                print(f"   [task] crashed: {e!r}")
                task.status, task.error, task.finished = "failed", str(e)[:200], time.time()
                task.add_log(f"error: {e}")
                self.solu.task_say(f"Sorry, I couldn't finish {task.title}. {short(str(e), 90)}")
            finally:
                self.current = None
                self.tls.task = None
                self.save()
                self.emit(True)

    def in_task(self):
        return getattr(self.tls, "task", None) is not None

    def _checkpoint(self, task):
        """Between steps: stop if cancelled, sit still while paused (or while the user wants privacy), give up if out of budget."""
        while True:
            if task.status == "cancelled":
                raise Cancelled()
            if task.status == "paused" or getattr(self.solu, "paused", False):
                self.emit()
                with self.cond:
                    self.cond.wait(timeout=0.7)
                continue
            if task.actions >= task.budget or time.time() > task.deadline:
                self._park(task, "budget", f"I've used my budget on {task.title}: {task.actions} actions so far. Should I keep going?")
            return

    def _apply_reply(self, task):
        """The user answered what the task asked while it was parked: act on it."""
        pending, reply = task.pending, task.reply or ""
        task.pending = task.reply = None
        low = reply.lower()
        kind = pending["kind"]
        if kind == "budget":
            if re.search(r"\b(yes|yeah|sure|go on|keep|continue|carry|ok|okay)\b", low):
                task.budget, task.deadline = task.actions + MAX_ACTIONS // 2, time.time() + 45 * 60
            else:
                task.status = "paused"
                task.add_log("paused: budget used")
        elif kind == "stuck":
            step = task.steps[pending["step"]] if pending.get("step") is not None and pending["step"] < len(task.steps) else None
            if re.search(r"\b(stop|cancel|quit|give up|abort|forget it)\b", low):
                task.status, task.finished = "cancelled", time.time()
                task.add_log("stopped after getting stuck")
            elif step is not None and re.search(r"\b(skip|move on|next)\b", low):
                step["status"] = "skipped"
            elif step is not None:
                step["status"], step["tries"] = "todo", 0
                task.notes["user advice"] = short(reply, 200)
                task.hint = f"The user advised: {reply}"
        else:                                                                       # ask_user
            task.notes["Q: " + short(pending["question"], 60)] = short(reply, 200)
            task.hint = (f"You asked the user \"{pending['question']}\" and they answered: {reply}. "
                         f"If this step was only to get that answer, call step_done now.")
        self.save()

    def _park(self, task, kind, question, step=None):
        """Ask the user something and get out of the way: the task waits, the worker serves other tasks, the answer resumes it."""
        with self.cond:
            task.pending = {"kind": kind, "question": question, "step": step, "asked": time.time()}
            task.reply, task.status, task.question = None, "waiting", question
            task.add_log(f"asked: {short(question, 100)}")
            self.save()
        self.emit(True)
        self.solu.task_say(question, listen=True)
        raise Parked()

    def _run(self, task):
        if task.pending and task.reply is not None:
            self._apply_reply(task)
            if task.status in ("paused", "cancelled"):
                return
        if task.status == "planning":
            self.emit(True)
            self._plan(task)
            self._checkpoint(task)
        if task.status == "planning":                                              # no plan card: the user's rule is to just do it
            task.status = "running"
            self.save()
        self.emit(True)
        while True:
            self._checkpoint(task)
            step = task.current()
            if step is None:
                break
            ok, why = self._do_step(task, step)
            self.save()
            self.emit(True)
            if ok:
                continue
            step["tries"] = step.get("tries", 0) + 1
            if step["tries"] == 1 and task.replans < 2:
                step["status"] = "failed"
                if self._replan(task, step, why):
                    continue
            if not self._recover(task, step, why):
                return
        self._finish(task)

    # ---- planning -----------------------------------------------------------------------------------------------------
    def _plan(self, task):
        plan = _json(self._llm([{"role": "user", "content": PLAN.format(goal=task.goal)}], json_mode=True, max_tokens=700))
        steps = [s for s in plan.get("steps", []) if isinstance(s, dict) and s.get("text")][:9]
        if not steps:
            raise RuntimeError("I couldn't work out a plan for that")
        task.title = short(plan.get("title") or task.goal, 44)
        task.needs = [str(n) for n in plan.get("needs", [])][:8]
        task.steps = [{"text": short(s["text"], 200), "done_when": short(s.get("done_when", ""), 160), "status": "todo", "result": "", "tries": 0} for s in steps]
        task.add_log(f"planned {len(task.steps)} steps")
        self.save()

    def _replan(self, task, failed, why):
        done = "; ".join(s["text"] for s in task.steps if s["status"] == "done") or "nothing yet"
        out = _json(self._llm([{"role": "user", "content": REPLAN.format(goal=task.goal, done=done, step=failed["text"], why=short(why, 300),
                                                                        notes=json.dumps(task.notes)[:400])}], json_mode=True, max_tokens=600))
        steps = [s for s in out.get("steps", []) if isinstance(s, dict) and s.get("text")][:8]
        if not steps:
            return False
        kept = [s for s in task.steps if s["status"] in ("done", "skipped")]
        task.steps = kept + [{"text": short(s["text"], 200), "done_when": short(s.get("done_when", ""), 160), "status": "todo", "result": "", "tries": 1} for s in steps]
        task.replans += 1
        task.add_log(f"changed the plan after: {short(why, 120)}")
        return True

    def _recover(self, task, step, why):
        """Still stuck: ask the user what to do. Returns True to carry on."""
        index = task.steps.index(step)
        self._park(task, "stuck", f"I'm stuck on {short(step['text'], 70)}: {short(why, 90)}. Should I skip it, try again, or stop?", step=index)

    def _finish(self, task):
        steps = "\n".join(f"- {s['text']} => {s['status']}{': ' + short(s['result'], 100) if s['result'] else ''}" for s in task.steps)
        failed = [s for s in task.steps if s["status"] == "failed"]
        try:
            text = self._llm([{"role": "user", "content": SUMMARY.format(goal=task.goal, steps=steps)}], max_tokens=120).strip()
        except Yield:
            text = ""
        task.result = text or f"I finished {task.title}."
        task.status = "failed" if failed and not task.done_count else "done"
        task.finished = time.time()
        task.add_log("finished")
        self.save()
        self.emit(True)                                                             # the island shows "done" now, not after the result is spoken
        self.solu.event("activity_end", id="task", done=task.status == "done", title="Task done" if task.status == "done" else "Task failed",
                        detail=task.title)
        self.solu.task_say(task.result)

    # ---- one step -----------------------------------------------------------------------------------------------------
    def _do_step(self, task, step):
        step["status"] = "doing"
        gui = bool(GUI_WORDS.search(step["text"]))
        screen_text = self._observe() if gui else ""
        used_gui, hint, repeats, texts, last_sig, last_obs, verified, same = False, task.hint, 0, 0, None, None, 0, 0
        task.hint, notes = "", 0
        self._last_change = ""
        tools = self._tools_for(task, step)
        for _ in range(MAX_STEP_ACTIONS + 6):                                        # + room for a few notes, which are not actions
            self._checkpoint(task)
            messages = [{"role": "system", "content": SYSTEM}, {"role": "user", "content": self._turn(task, step, screen_text, hint)}]
            hint = ""
            t_llm = time.monotonic()
            text, calls = self._llm(messages, tools=tools, max_tokens=320, want_calls=True)
            t_llm = time.monotonic() - t_llm
            if not calls:
                texts += 1
                hint = "Call a tool now. If the step is complete call step_done; if impossible call give_up."
                if texts >= 3:
                    return False, "the worker did not choose an action"
                continue
            texts = 0
            batch = [self._call_of(c) for c in calls[:MAX_BURST + 1]]
            todo = [(n, a) for n, a in batch if n not in CONTROL_TOOLS][:MAX_BURST]
            if todo:                                                                 # actions first; a step_done sent with them is ignored:
                for n, a in batch:                                                   # it was claimed before the result was seen
                    if n == "note":
                        task.notes[short(a.get("key", "note"), 30)] = short(a.get("value", ""), 300)
            else:
                name, args = batch[0]
                if name == "step_done":
                    proof = short(args.get("proof", ""), 200)
                    if used_gui and step.get("done_when"):                           # every claim is checked against the screen
                        ok, why = self._verify(step, proof)
                        if not ok:
                            verified += 1
                            task.add_log(f"check failed: {why}")
                            if verified >= 2:                                        # claimed twice, never shown: the step failed
                                return False, f"it said the step was done, but the screen did not show it: {why}"
                            hint = f"Not done yet: {why}"
                            continue
                    step["status"], step["result"] = "done", proof
                    task.add_log(f"done: {short(step['text'], 80)}")
                    return True, ""
                if name == "give_up":
                    return False, short(args.get("reason", "it could not be done"), 200)
                if name == "ask_user":
                    question = args.get("question", "What should I do?")
                    asked = task.notes.get("Q: " + short(question, 60))
                    if asked is not None:                                            # never ask the same thing twice
                        hint = f"You already asked that and the user answered: {asked}. Use it. If this step only needed that answer, call step_done."
                        continue
                    self._park(task, "ask", question)
                key, value = short(args.get("key", "note"), 30), short(args.get("value", ""), 300)    # a note
                notes += 1
                if task.notes.get(key) == value or notes >= 4:                       # notes are free, but not an endless loop
                    if notes >= 6:
                        return False, "the worker kept writing notes instead of acting"
                    hint = "That is already in the notes. Now act: do the step, or call step_done if it is already complete."
                task.notes[key] = value
                continue
            # ---- a burst: one action, or a short sequence the model was sure of, back to back. Every action still passes the
            # window guard and the approvals; the burst stops at the first one that fails, and the screen is looked at once, at the end.
            sig = hashlib.md5(json.dumps(todo, sort_keys=True).encode()).hexdigest()
            t_exec = time.monotonic()
            done_now, results, gui_now = [], [], False
            for i, (name, args) in enumerate(todo):
                if i:
                    self._checkpoint(task)                                           # pause / stop work between actions too
                result = self._exec(task, name, args)
                task.actions += 1
                gui_now |= name in GUI_TOOLS
                task.add_log(f"{name}({short(json.dumps(args, ensure_ascii=False), 90)}) -> {short(result, 120)}")
                done_now.append(name)
                results.append((name, result))
                failed = result.lower().startswith(("error", "skipped", "the user said no", "the user did not", "could not"))
                if not failed and (name in ("open_app", "open_website") or (name == "window_control" and args.get("action") == "focus")):
                    front = ctypes.windll.user32.GetForegroundWindow()
                    if front:
                        task.hwnd = front                                            # the task brought this window up itself: work in it
                        if name != "window_control" and front not in task.own:
                            task.own.append(front)                                   # a window it opened: it may close it later
                if failed:
                    break
                if i < len(todo) - 1 and name in GUI_TOOLS:
                    time.sleep(SETTLE_BETWEEN)                                       # let the app take the input before the next one
            used_gui |= gui_now
            t_exec = time.monotonic() - t_exec
            t_obs = time.monotonic()
            if gui_now:
                time.sleep(SETTLE)                                                   # let the window settle, then look once
                screen_text = self._observe(remember=True)
                last_name, last_result = results[-1]
                prefix = "[CHANGED after your action: "
                change = screen_text[len(prefix):screen_text.find("]" + chr(10))] if screen_text.startswith(prefix) else ""
                if not hint and change:
                    hint = (f"Your {last_name} changed the screen: {change}. If that means the CURRENT step is done, call step_done now "
                            f"(do not repeat the action).")
                elif not hint and last_result.startswith("error: could not find") and getattr(self, "_last_change", ""):
                    hint = (f"That target is gone because your earlier action changed the screen ({self._last_change}). "
                            f"It probably worked: if the CURRENT step is done, call step_done.")
                if change:
                    self._last_change = change
                data = [(n, r) for n, r in results if n not in GUI_TOOLS and n != "look"]
                if data:
                    screen_text = "\n".join(f"[result of {n}]\n{r[:900]}" for n, r in data) + "\n" + screen_text
            elif results[-1][0] == "look":
                screen_text = results[-1][1]
            else:                                                                    # research, files, a command's output, an agent's reply: shown in full
                screen_text = "\n".join(f"[result of {n}]\n{r[:2200 // len(results)]}" for n, r in results)
            if len(done_now) < len(todo):
                hint = (f"Only {len(done_now)} of the {len(todo)} actions you sent were done: {done_now[-1]} said "
                        f"\"{short(results[-1][1], 140)}\". The rest were NOT done.")
            t_obs = time.monotonic() - t_obs
            # Stuck = the same actions AND the screen did not change. (Clicking Next again after the page changed is not stuck.)
            obs = hashlib.md5(screen_text.encode()).hexdigest()
            repeats = repeats + 1 if (sig == last_sig and obs == last_obs) else 0
            same = same + 1 if sig == last_sig else 0
            last_sig, last_obs = sig, obs
            if same >= 3:
                hint = "You have done this exact action several times. Look at what changed: if the step's goal is already met, call step_done now."
                if same >= 6:
                    return False, "the same action was repeated many times"
            if repeats >= 2:
                hint = "You repeated the same action and the screen did not change. Try something different, or call give_up."
                if repeats >= 4:
                    return False, "the same action kept having no effect"
            print(f"   [task] {'+'.join(done_now)}: model {t_llm:.1f}s, actions {t_exec:.2f}s, look {t_obs:.1f}s")
            self.emit()
        return False, "too many actions without finishing the step"

    def _tools_for(self, task, step):
        wanted = (set(BASE_TOOLS) | {t["function"]["name"] for t in actions.pick_tools(step["text"] + " " + task.goal)}) - {"start_task"}
        chosen = [t for t in actions.TOOLS if t["function"]["name"] in wanted]
        return TOOLS + chosen

    def _turn(self, task, step, screen_text, hint):
        plan = "\n".join(f"{'x' if s['status'] in ('done', 'skipped') else '>' if s is step else ' '} {i + 1}. {s['text']}" for i, s in enumerate(task.steps))
        recent = "\n".join(f"- {e['text']}" for e in task.log[-6:] if not e["text"].startswith(("planned", "started")))
        notes = "; ".join(f"{k}: {v}" for k, v in task.notes.items()) or "(none)"
        return (f"GOAL: {task.goal}\nPLAN:\n{plan}\nCURRENT STEP: {step['text']}\nDONE WHEN: {step.get('done_when') or 'the step is clearly complete'}\n"
                f"NOTES: {notes}\nRECENT:\n{recent or '(nothing yet)'}\n{FOLDERS}\n"
                f"SCREEN:\n{screen_text or '(not looked at yet; call look if you need to)'}\n"
                f"{('NOTE: ' + hint) if hint else ''}\nWhat is the next action?")

    @staticmethod
    def _call_of(call):
        fn = call.get("function", {})
        args = fn.get("arguments") or {}
        if isinstance(args, str):
            try:
                args = json.loads(args)
            except ValueError:
                args = {}
        return fn.get("name", ""), args if isinstance(args, dict) else {}

    def _observe(self, remember=False):
        try:
            text = self.solu.hands.observe()
            view = getattr(self.solu.hands, "_view", None)
            lines = [" ".join(l.split()) for l in (view["look"].text if view else "").splitlines() if l.strip()]
            if remember:
                before, self._prev_lines = getattr(self, "_prev_lines", None), lines
                if before is not None:                                                # tell the model what its action did
                    old, new = set(before), set(lines)
                    added = [l for l in lines if l not in old][:5]
                    gone = [l for l in before if l not in new][:5]
                    if added or gone:
                        diff = "; ".join([f"now shows \"{short(l, 70)}\"" for l in added] + [f"no longer shows \"{short(l, 70)}\"" for l in gone])
                        text = f"[CHANGED after your action: {diff}]\n" + text
                    else:
                        text = "[the screen did NOT change after your action]\n" + text
                if self.current is not None:
                    import ctypes
                    self.current.hwnd = ctypes.windll.user32.GetForegroundWindow()   # actions go to this window and no other
            else:
                self._prev_lines = lines
            return text
        except Exception as e:                                                      # noqa: BLE001
            return f"(could not look: {e})"

    @staticmethod
    def _named_in_goal(task, hwnd, title=None):
        """The window's title (any " - " part of it) or its program is mentioned in what the USER asked for (the goal, not the plan)."""
        import screen
        goal = task.goal.lower()
        title = title if title is not None else actions._foreground_title()
        parts = [p.strip().lower() for p in re.split(r"\s[-—–]\s", title) if len(p.strip()) >= 4]
        app = (screen._process_name(hwnd) or "").lower()
        return any(p in goal for p in parts) or (len(app) >= 4 and app in goal)

    def _verify(self, step, claim):
        out = _json(self._llm([{"role": "user", "content": VERIFY.format(step=step["text"], done_when=step.get("done_when", ""), claim=claim,
                                                                       screen=self._observe()[:1400])}], json_mode=True, max_tokens=90))
        return bool(out.get("done", True)), short(out.get("why", ""), 160)

    # ---- doing it (with the approvals the user gave) ------------------------------------------------------------------
    def _exec(self, task, name, args):
        if name not in actions.TOOL_NAMES:
            return f"error: there is no tool called {name}"
        if name in GUI_ACTIONS:                                                      # never type or click into a window the task did not choose
            import ctypes
            front = ctypes.windll.user32.GetForegroundWindow()
            if task.hwnd and front != task.hwnd:
                return (f"error: nothing was done: the window in front is now '{actions._foreground_title()[:60]}', not the one this task was working in "
                        f"(did something else come up?). Use look to see it, and window_control focus to bring the right window back.")
            if not task.hwnd:                                                         # the first action: the window must be one the goal talks about
                if front not in task.own and not self._named_in_goal(task, front):
                    return (f"error: nothing was done: the window in front is '{actions._foreground_title()[:60]}' and the task never chose it. "
                            f"Bring the right window to the front first (window_control focus, or open_app).")
                task.hwnd = front
        if name == "window_control" and args.get("action") not in ("list", "focus", None):
            # closing / moving / resizing: only a window this task opened, or one the user named in the goal (a task once closed
            # the user's YouTube window because its title was on the screen)
            hwnd, title = actions._find_window(args.get("title", "")) if args.get("title") else (ctypes.windll.user32.GetForegroundWindow(), actions._foreground_title())
            if not hwnd:
                return f"error: no open window matches '{args.get('title', '')}'"
            if hwnd not in task.own and not self._named_in_goal(task, hwnd, title):
                return (f"error: nothing was done: '{short(title, 60)}' is one of the user's own windows, not one this task opened or the goal "
                        f"names. Leave it alone and carry on with the task's own windows.")
        if name == "type_text":                                                      # typing is not idempotent: never type what is already there
            want = " ".join(str(args.get("text", "")).split()).lower()
            seen = " ".join(getattr(self, "_prev_lines", None) or []).lower()
            if len(want) >= 3 and want in seen and name in self._typed.get(task.id, ""):
                return "skipped: that exact text is already on the screen from your last typing, so it was not typed again. Check the screen and call step_done."
            self._typed[task.id] = self._typed.get(task.id, "") + "|" + name
        for key in ("path", "folder", "source", "destination"):                     # "my Documents" is the user's, not the shared Public one
            value = args.get(key)
            if isinstance(value, str) and re.match(r"(?i)^[a-z]:\\users\\public\\", value) and "public" not in task.goal.lower():
                args[key] = os.path.join(actions.HOME, value.split("\\", 3)[3])
        if actions.needs_approval(name, args):                                     # deleting, writing, closing, sending: asked every time
            answer = self.solu.ask_permission(name, args, background=True)
            if answer == "deny":
                return "the user said no, so it was not done; do not try another way to do the same thing"
            if answer == "timeout":
                return "the user did not answer, so it was not done"
        self.solu.event("action", tool=name)
        return self.solu.hands.run(name, args)

    # ---- the model, politely --------------------------------------------------------------------------------------------
    def _llm(self, messages, tools=None, json_mode=False, max_tokens=350, want_calls=False):
        """One model call that never gets in the user's way: it waits while the conversation is busy and is dropped when they speak."""
        for attempt in range(40):
            while self.solu.chat_busy():
                self._pause_check()
                time.sleep(0.25)
            try:
                return self._llm_once(messages, tools, json_mode, max_tokens, want_calls)
            except Yield:
                continue
        raise Yield()

    def _pause_check(self):
        task = self.tls.task
        if task is not None and task.status == "cancelled":
            raise Cancelled()

    def _llm_once(self, messages, tools, json_mode, max_tokens, want_calls):
        body = gpu.body(self.model, {"num_ctx": self.ctx, "temperature": 0.2, "num_predict": max_tokens}, stream=True, think=False, messages=messages)
        if tools:
            body["tools"] = tools
        if json_mode:
            body["format"] = "json"
        req = urllib.request.Request(self.ollama, json.dumps(body).encode(), {"Content-Type": "application/json"})
        text, calls = "", []
        with urllib.request.urlopen(req, timeout=300) as resp:
            for line in resp:
                if self.solu.chat_busy():                                            # the user started to talk: let them have the model
                    raise Yield()
                self._pause_check()
                msg = json.loads(line).get("message", {})
                text += msg.get("content", "")
                calls += msg.get("tool_calls") or []
        if want_calls:
            if not calls and "<tool_call>" in text:                                  # a call written as text
                clean, written = self.solu.text_tool_calls(text)
                text, calls = clean, written
            return text, calls
        return text

    # ---- schedules: "every morning at 8, ..." -----------------------------------------------------------------------------
    def add_schedule(self, goal, at, days):
        with self.lock:
            self.schedules.append({"id": uuid.uuid4().hex[:6], "goal": goal, "time": at, "days": days, "last": ""})
            self.save()

    def _check_schedules(self):
        now = datetime.datetime.now()
        with self.lock:
            for s in self.schedules:
                if now.weekday() in s["days"] and now.strftime("%H:%M") >= s["time"] and s.get("last") != now.strftime("%Y-%m-%d"):
                    if not any(t.schedule_id == s["id"] and t.status in ("planning", "running", "waiting", "paused") for t in self.tasks):
                        s["last"] = now.strftime("%Y-%m-%d")
                        threading.Thread(target=self.submit, args=(s["goal"], s["id"]), daemon=True).start()
            self.save()

    def list_schedules(self):
        with self.lock:
            return "; ".join(f"{s['goal']} at {s['time']}" for s in self.schedules) or "none"

    def remove_schedule(self, words):
        import difflib
        with self.lock:
            hit = difflib.get_close_matches(words.lower(), [s["goal"].lower() for s in self.schedules], n=1, cutoff=0.4)
            if hit:
                self.schedules = [s for s in self.schedules if s["goal"].lower() != hit[0]]
                self.save()
                return hit[0]
        return None


DAYS = {"monday": 0, "tuesday": 1, "wednesday": 2, "thursday": 3, "friday": 4, "saturday": 5, "sunday": 6}


def parse_schedule(text):
    """'every morning at 8 tidy my downloads' -> (goal, 'HH:MM', [weekdays]); None if it is not a recurring request."""
    m = re.match(r"(?i)^(?:solu,?\s*)?(?:please\s+)?every\s+(day|morning|evening|night|weekday|weekdays|monday|tuesday|wednesday|thursday|friday|saturday|sunday)\s*"
                 r"(?:at\s+(\d{1,2})(?::(\d{2}))?\s*(am|pm)?)?\s*,?\s*(.{6,})$", text.strip())
    if not m:
        return None
    when, hour, minute, ampm, goal = m.groups()
    if hour is None:
        hour, minute = {"morning": 8, "evening": 18, "night": 21}.get(when.lower(), 9), 0
    else:
        hour, minute = int(hour), int(minute or 0)
        if ampm and ampm.lower() == "pm" and hour < 12:
            hour += 12
        if ampm and ampm.lower() == "am" and hour == 12:
            hour = 0
    w = when.lower()
    days = [DAYS[w]] if w in DAYS else [0, 1, 2, 3, 4] if w.startswith("weekday") else list(range(7))
    return goal.strip(), f"{hour:02d}:{minute:02d}", days
