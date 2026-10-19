"""Approval cards: before a risky action Solu asks on the island (or by voice) and waits for the answer."""
import re
import threading
import time

import actions


YES = re.compile(r"\b(yes|yeah|yep|yup|sure|ok|okay|go ahead|do it|allow|approve|approved|please do|of course|fine|go on|alright)\b")
NO = re.compile(r"\b(no|nope|don't|dont|do not|cancel|stop|never ?mind|wait|not now|deny|reject)\b")
ALL = re.compile(r"\b(yes to (all|everything)|(do|allow) (it )?all|all of (it|them)|everything|the whole thing|all (the )?steps|go ahead with (it )?all)\b")
ALWAYS = re.compile(r"\b(always|every time|don't ask( me)? again|dont ask)\b")


class ApprovalsMixin:
    """Asking the user before acting. Mixed into Solu."""

    # ---- cards -----------------------------------------------------------------------------------------------
    def ask_permission(self, name, args, background=False):
        """Show the approval card and wait for a tap (or a spoken yes / no). Returns allow, all, deny or timeout.
        background: asked by a task, not in the middle of a conversation turn: it waits for the conversation, leaves its flags alone."""
        action, detail, risky = actions.describe(name, args)
        if background:
            return self._ask_permission_background(name, action, detail, risky)
        self._wait_for_playback()                                             # let Solu finish its sentence first
        self._approval_seq += 1
        aid = self._approval_seq
        waiter = threading.Event()
        self.approvals[aid] = {"event": waiter, "answer": None}
        self.event("approve", id=aid, action=action, detail=detail, risky=risky)
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

    def _ask_permission_background(self, name, action, detail, risky):
        t0 = time.monotonic()
        while self.chat_busy() and time.monotonic() - t0 < 120:               # not on top of what is being said
            time.sleep(0.3)
        self._approval_seq += 1
        aid = self._approval_seq
        waiter = threading.Event()
        self.approvals[aid] = {"event": waiter, "answer": None}
        self.event("approve", id=aid, action=action, detail=detail, risky=risky)
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
        """allow / all / deny for a sentence said while an approval card is up."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        if ALWAYS.search(t):                                                  # "always" / "don't ask again" is a yes (checked
            return "allow"                                                    # first: "don't" would read as "no")
        if NO.search(t):
            return "deny"
        if ALL.search(t):
            return "all"
        if YES.search(t):
            return "allow"
        return None                                                           # not an answer (a video, someone else talking): ignore it
