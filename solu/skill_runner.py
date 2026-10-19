"""Solu's side of the skills in skills.py: routines, the study quiz, lecture notes, the morning briefing, messages."""
import json
import os
import re
import threading
import time

import actions
import screen as screen_mod
import skills


class SkillsMixin:
    """Running skills from a conversation. Mixed into Solu."""

    # ---- routines --------------------------------------------------------------------------------------------
    def run_routine(self, name):
        steps = self.routines.items[name]
        print(f"   [routine] {name}: {steps}")
        try:
            self.cancel.clear()
            self._speaking_reported = False
            self.speaking.set()
            self.allow_all_next = True                                      # the user wrote this routine: its safe steps need no cards
            self._agent(f"Do all of these now, one after another: {steps}")
        finally:
            self._after_speaking()


    # ---- study buddy -----------------------------------------------------------------------------------------
    def start_quiz_from(self, source=""):
        src = (source or "").strip()
        text, where = "", ""
        if src and not re.search(r"\b(this|screen|page|pdf|document|window|it)\b", src.lower()):
            text, where = actions.research(src), src                         # a topic: learn it from the web first
        else:
            look = screen_mod.look_at_foreground()
            if look.blocked:
                return "error: " + look.blocked
            path = None
            if look.title.lower().split(" - ")[0].strip().endswith(".pdf"):  # a PDF reader: read the whole file, not just the page
                name = look.title.split(" - ")[0].strip()
                found = actions.search_files(name).splitlines()
                path = next((ln.replace("[folder] ", "") for ln in found if ln.lower().endswith(".pdf")), None)
            if path and os.path.exists(path):
                text, where = skills.pdf_text(path), os.path.basename(path)
            else:
                text, where = screen_mod.read_text(look), look.title
        if len(text) < 200:
            return "error: there isn't enough text to make a quiz from; open the page or PDF first, or name a topic"
        data = json.loads(self._ask(skills.QUIZ.format(n=5, text=text[:9000]), True))
        questions = [q for q in data.get("questions", []) if q.get("q") and q.get("a")][:5]
        if not questions:
            return "error: I couldn't make questions from that"
        self.quiz = skills.Quiz(questions, data.get("topic") or where)
        return (f"Quiz ready on {self.quiz.topic}: {len(questions)} questions. Tell the user you'll quiz them, then ask question 1 "
                f"exactly as written: {questions[0]['q']}")

    def quiz_answer(self, answer):
        quiz = self.quiz
        q = quiz.current
        if not q:
            return self.end_quiz()
        try:
            verdict = json.loads(self._ask(skills.GRADE.format(q=q["q"], a=q["a"], answer=answer), True))
        except Exception:                                                     # noqa: BLE001
            verdict = {"correct": False, "feedback": f"The answer was: {q['a']}"}
        if verdict.get("correct"):
            quiz.score += 1
        else:
            quiz.missed.append(q.get("point") or q["q"])
        quiz.index += 1
        line = verdict.get("feedback", "")
        if quiz.current:
            line += f" Question {quiz.index + 1}: {quiz.current['q']}"
            self.respond_text(line)
        else:
            self.respond_text(line)
            self.end_quiz()

    def end_quiz(self):
        quiz, self.quiz = self.quiz, None
        if not quiz:
            return
        asked = max(1, quiz.index)
        text = f"That's the quiz! You got {quiz.score} out of {asked}."
        if quiz.missed:
            text += " Worth reviewing: " + ", ".join(quiz.missed[:3]) + "."
            self.memory.add(f"In a quiz on {quiz.topic}, the user found these hard: {', '.join(quiz.missed[:3])}.", "plan", "learned")
        else:
            text += " Perfect, nice work!"
            self.event("cheer")
        self.respond_text(text)


    # ---- notes -----------------------------------------------------------------------------------------------
    def notes_start(self, kind="lecture"):
        if self.notes and self.notes.is_alive():
            return "error: I'm already taking notes; say stop taking notes when it's over"
        self.notes = skills.NoteTaker(self, kind)
        self.notes.start()
        self.event("activity", id="notes", title="Taking notes", detail=f"{kind} · listening to your PC")
        return f"started taking notes of the {kind} from the PC's sound. Tell the user you're listening and they should tell you when it's over."

    def _notes_start_line(self, kind):
        result = self.notes_start(kind)
        if result.startswith("error"):
            return "I'm already taking notes."
        return f"Okay, I'm listening to the {kind} on your PC. Tell me when it's over."      # no quoted phrase for the echo filter to trip on

    def _notes_stop_line(self):
        if not self.notes:
            return "I'm not taking notes right now."
        self.notes_stop()
        return "Okay, writing your notes now. They'll open in Chrome in a minute."

    def notes_stop(self):
        if not self.notes:
            return "error: I'm not taking notes right now"
        notes, self.notes = self.notes, None

        def finish():
            try:
                path, title, markdown = notes.finish()
                self.event("activity_end", id="notes", done=True, title="Notes ready", detail=title)
                self._wait_idle()
                self.respond_text(f"Your notes on {title} are ready. I've opened them in Chrome.")
                self.jobs.append(type("Done", (), {"status": "done", "title": title, "markdown": markdown, "path": path,
                                                  "started": time.time(), "is_alive": lambda self: False})())
            except Exception as e:                                            # noqa: BLE001
                self.event("activity_end", id="notes")
                self._wait_idle()
                self.respond_text(f"Sorry, I couldn't write the notes: {str(e)[:100]}")
        threading.Thread(target=finish, daemon=True).start()
        return "stopped recording; the notes are being written and will open in Chrome in a minute. Tell the user that."


    # ---- briefing and messages -------------------------------------------------------------------------------
    def briefing(self):
        self.cancel.clear()
        self._speaking_reported = False
        self.speaking.set()
        try:
            facts = [time.strftime("It is %A %d %B, %I:%M %p")]                # (not uptime or memory: that's no briefing)
            w = skills.weather()
            if w:
                facts.append("Weather: " + w)
            unread = skills.unread_summary(self.context)
            if unread:
                facts.append("Notifications waiting: " + unread)
            plans = [m["text"] for m in self.memory.items if m["kind"] == "plan" or skills.dates_in(m["text"])]
            if plans:
                facts.append("Plans the user mentioned: " + "; ".join(plans[-5:]))
            title, _, _ = self.latest_job()
            if title:
                try:
                    news = actions.search_results(f"{title} news")[:1]
                    if news:
                        facts.append(f"A recent headline about something they researched ({title}): {news[0][0]} - {news[0][2][:200]}")
                except Exception:                                             # noqa: BLE001
                    pass
            name = self._user_name()
            prompt = ("Give the user a warm, short spoken briefing (4 to 6 sentences) from these facts. Greet them for the time of day"
                      + (f" by name ({name})" if name else " without using any name (you don't know it)") +
                      "; mention the weather, anything waiting for them and their plans; end with one encouraging line. "
                      "Only mention rain if the chance is above 40%. Use only these facts:\n- " + "\n- ".join(facts) +
                      "\nWhat you know about the user:\n- " + "\n- ".join(self.memory.relevant("plans today")))
            self.history.append({"role": "user", "content": "Good morning! Brief me."})
            text = self._ask(prompt).strip()
            self.history.append({"role": "assistant", "content": text})
        except Exception as e:                                                # noqa: BLE001
            text = f"Good morning! I couldn't put the whole briefing together just now ({str(e)[:60]})."
        finally:
            self.speaking.clear()
        self.respond_text(text)

    def smart_send(self, to, message, app=""):
        """Find the person (memory, WhatsApp search or Gmail's suggestions), show the card, then send."""
        to, app = to.strip(), (app or "").lower()
        address = actions.email_address(to)
        known = " ".join(self.memory.relevant(to))
        if not address and app != "whatsapp":                                 # an email address remembered for this person?
            m = re.search(r"[A-Za-z0-9._%+\-]+@[A-Za-z0-9\-.]+\.[A-Za-z]{2,}", known) if to.lower().split()[0] in known.lower() else None
            if m:
                address = m.group(0)
        if not app:
            app = "email" if address or "email" in known.lower() or "gmail" in known.lower() else "whatsapp"
        if app in ("email", "gmail", "mail"):
            if not address:
                address, note = skills.gmail_lookup(to)
                if not address:
                    return f"error: {note}. Ask the user for {to}'s email address."
            subject = message.split(".")[0][:60] if len(message) > 80 else "Message from " + (self._user_name() or "me")
            answer = self.ask_permission("send_email", {"to": address, "subject": subject, "body": message})
            if answer not in ("allow", "always", "all"):
                return "the user said no. Do not send it; just say okay."
            result = actions.send_gmail(address, subject, message)
            self.memory.add(f"{to.title()}'s email address is {address}.", "person", "learned")
            return result
        chat, note = skills.whatsapp_open_chat(to)
        if not chat:
            return f"error: {note}"
        answer = self.ask_permission("whatsapp", {"to": chat, "text": message})
        if answer not in ("allow", "always", "all"):
            return "the user said no. Do not send it; just say okay."
        skills.whatsapp_type_and_send(message)
        self.memory.add(f"The user messages {chat} on WhatsApp.", "person", "learned")
        return f"sent to {chat} on WhatsApp"

    def _user_name(self):
        for m in self.memory.items:
            if m["kind"] == "name":
                return m["text"].rsplit(" ", 1)[-1].rstrip(".")
        return ""
