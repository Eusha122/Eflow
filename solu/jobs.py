"""Background work: deep-research jobs and the hooks long tasks use to talk without interrupting a conversation."""
import re
import threading
import time

import research as research_mod


class JobsMixin:
    """Research reports and the task runner's hooks. Mixed into Solu."""

    # ---- research --------------------------------------------------------------------------------------------
    def start_research(self, topic, depth="deep"):
        running = [j for j in self.jobs if j.is_alive()]
        if running:
            return f"error: research on '{running[0].topic}' is still running ({running[0].status}); wait for it to finish"
        job = research_mod.Job(topic, depth, self._ask, self._wait_idle, self._research_done)
        self.jobs = (self.jobs + [job])[-5:]
        job.start()

        def follow():                                                         # the island shows it while it works
            while job.is_alive():
                self.event("activity", id="research", title="Researching", detail=f"{job.topic} · {job.status}",
                           progress=round(max(0.03, job.progress), 2))
                time.sleep(2)
        threading.Thread(target=follow, daemon=True).start()
        minutes = "3 to 6" if job.depth == "deep" else "1 to 3"
        return (f"started {job.depth} research on '{job.topic}' in the background (about {minutes} minutes). Tell the user you're on it, "
                f"that they can keep talking to you, and that the report will open in Chrome when it's ready.")

    def _wait_idle(self):
        while self.speaking.is_set():                                         # the conversation always goes first
            time.sleep(0.3)

    def _research_done(self, job):
        self.event("activity_end", id="research", done=not job.error, title="Research ready", detail=job.title)
        if job.error:
            text = f"Sorry, the research on {job.topic} didn't work out. {job.error[:80]}"
        else:
            text = f"Your research on {job.title} is ready. I've opened it in Chrome. Want me to read you the summary?"
        self._wait_idle()
        self.respond_text(text)

    def latest_job(self):
        done = [j for j in self.jobs if j.status == "done"]
        if done:
            return done[-1].title, done[-1].markdown, done[-1].path
        path, text = research_mod.latest_report()
        if not path:
            return None, "", None
        title = text.splitlines()[0].lstrip("# ").strip() if text else "your last research"
        return title, text, path

    @staticmethod
    def report_section(markdown, name):
        m = re.search(rf"##\s*{name}\s*\n(.+?)(?=\n##\s|\Z)", markdown, re.S | re.I)
        return m.group(1).strip() if m else ""

    @staticmethod
    def for_reading(markdown):
        """Report text as it should be read aloud: no markup, no source numbers, no source list."""
        text = re.split(r"\n##\s*Sources", markdown)[0]
        text = re.sub(r"\s*\[\d+(?:[,\-–\s]*\d+)*\]", "", text)
        text = re.sub(r"^#+\s*(.+)$", r"\1.", text, flags=re.M)
        text = re.sub(r"^[-*•]\s+", "", text, flags=re.M)
        lines, out = [ln.strip() for ln in re.sub(r"[*_`]", "", text).splitlines()], []
        for ln in lines:
            if ln and (not out or ln != out[-1]):                             # a repeated title is read once
                out.append(ln)
        return "\n".join(out)


    # ---- task hooks ------------------------------------------------------------------------------------------
    def chat_busy(self):
        """True while Solu talks, the user talks, or a card waits for an answer: background work must not use the model then."""
        return self.speaking.is_set() or time.monotonic() < self.chat_priority_until or self.pending_approval is not None

    def task_cancelled(self):
        return bool(self.tasks and self.tasks.in_task() and self.tasks.cancelled())

    def task_say(self, text, listen=False):
        """A task says something (a question, the result): after whatever is being said now. listen: the next sentence is the answer."""
        if not text:
            return
        t0 = time.monotonic()
        while self.chat_busy() and time.monotonic() - t0 < 120:
            time.sleep(0.3)
        if listen:
            self.start_conversation()
        self.respond_text(text)
