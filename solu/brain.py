"""Thinking: streaming answers from the language model, and the agent loop that lets it use tools on the PC."""
import json
import re
import threading
import time
import urllib.request

from config import BRAIN, CTX, FAST, OLLAMA, VISION
from prompts import AGENT_NOTE, SCREEN_NOTE, SYSTEM
import actions
import gpu
import memory as memory_mod


RECENT = re.compile(r"\b(who won|winner|champion|last|latest|current|recent|newest|this year|today'?s?|right now|news|price|cost|score|"
                    r"president|prime minister|ceo|released?|20[2-3]\d)\b", re.I)    # facts newer than the brain: searched first
EMOJI = re.compile(r"\s*[\U0001F000-\U0001FAFF☀-➿️]+")
PROMISE = re.compile(r"\b(let me(?! know)|i'll(?! be)|i will|i'm going to|i am going to|checking|one moment|one sec|give me a (sec|second)|next,? i)\b|"
                     r"^(opening|launching|starting|searching|setting|muting|unmuting|turning|playing|pausing|skipping|creating|sending|"
                     r"moving|closing|changing|adjusting|raising|lowering)\b")      # "Opening YouTube." with no call: nothing happened


class BrainMixin:
    """Answers from the language model, with tools. Mixed into Solu."""

    # ---- answers ---------------------------------------------------------------------------------------------
    def respond(self, user_text, screen=None):
        try:
            self._respond(user_text, screen)
        except Exception as e:                                                # noqa: BLE001  (Ollama not running, ...)
            print(f"   (the brain failed: {e})")
            self.event("error", message="Solu's brain (Ollama) did not answer. Is Ollama running?")
        finally:
            self._after_speaking()

    def _respond(self, user_text, screen=None):
        self.cancel.clear()
        self._speaking_reported = False
        self.speaking.set()
        if screen is None:
            return self._agent(user_text)
        else:                                                                # a question about the screen
            where = f"window \u201c{screen['title']}\u201d ({screen['app']})" if screen["scope"] == "window" else "the whole screen"
            seen = screen["text"][:3000]
            asked = f"{user_text}\n\n[What is on the user's screen: {where}. Text read from it, it may contain mistakes:]\n{seen}"
            before = self.recent(14, plain=True)                              # the conversation so far, words only
            self.history.append({"role": "user", "content": f"{user_text}\n[Screen: {where}: {screen['text'][:1200]}]"})   # kept short for follow-ups
            if screen["vlm"]:
                message = {"role": "user", "content": asked, "images": [screen["image"]]}
                body = gpu.body(VISION, {"num_ctx": 4096, "temperature": 0.4}, keep=0, stream=True, think=False,   # free the GPU right after
                                messages=[{"role": "system", "content": SYSTEM + screen.get("note", SCREEN_NOTE)}] + before[-4:] + [message])
            else:
                body = gpu.body(gpu.brain(BRAIN, FAST), {"num_ctx": CTX, "temperature": 0.4}, stream=True, think=False,
                                messages=[{"role": "system", "content": SYSTEM + screen.get("note", SCREEN_NOTE)}] + before + [{"role": "user", "content": asked}])
        reply, _, first_token = self._stream(body)
        print(f"   solu: {reply.strip()}   (brain first token {1000 * (first_token or 0):.0f} ms)")
        self.history.append({"role": "assistant", "content": reply.strip()})
        self.spoken = (self.spoken + [reply.strip()])[-3:]
        self.event("said", text=reply.strip())
        self._wait_for_playback()
        if screen["vlm"]:
            threading.Thread(target=self._load_brain, daemon=True).start()     # the vision model left: bring the brain back

    def _stream(self, body):
        """Ask the model, speaking its words as they arrive. Returns (text, tool calls, seconds to the first token)."""
        req = urllib.request.Request(OLLAMA, json.dumps(body).encode(), {"Content-Type": "application/json"})
        reply, buf, first_chunk, t0, calls, mute = "", "", True, time.perf_counter(), [], False
        sentence_end = re.compile(r"(?<=[.!?…])\s+")
        first_token = None
        with urllib.request.urlopen(req) as resp:
            for line in resp:
                if self.cancel.is_set():
                    break
                message = json.loads(line).get("message", {})
                calls += message.get("tool_calls") or []
                piece = message.get("content", "")
                if piece and first_token is None:
                    first_token = time.perf_counter() - t0
                reply += piece
                buf += piece
                if mute or "<tool" in buf or "<|" in buf:                    # the model wrote a tool call as text: never say it
                    if not mute:
                        before = buf[:buf.find("<")]
                        if before.strip():
                            self.say(before)
                    mute, buf = True, ""
                    continue
                while True:                                                   # speak as soon as a chunk is complete
                    m = sentence_end.search(buf)
                    cut = m.end() if m else None
                    if first_chunk and not cut:                              # first chunk may end at a comma
                        c = re.search(r"[,;:—]\s", buf)
                        if c and len(buf[:c.end()].split()) >= 3:
                            cut = c.end()
                    if not cut:
                        break
                    chunk, buf = buf[:cut], buf[cut:]
                    first_chunk = False
                    self.say(chunk)
        if buf.strip() and not self.cancel.is_set() and not mute:
            self.say(buf)
        return reply, calls, first_token

    @staticmethod
    def text_tool_calls(reply):
        """Pull '<tool_call>{json}</tool_call>' (written as text instead of a real call) out of a reply: (clean text, calls)."""
        calls = []
        for block in re.findall(r"<tool_call>\s*(\{.*?\})\s*(?:</tool_call>|$)", reply, re.S):
            try:
                data = json.loads(block)
                calls.append({"function": {"name": data.get("name", ""), "arguments": data.get("arguments") or {}}})
            except ValueError:
                pass
        clean = re.sub(r"<tool_call>.*?(</tool_call>|$)|<\|[^>]*\|>", "", reply, flags=re.S).strip()
        return clean, calls

    def recent(self, n, plain=False):
        """The last messages of the conversation, starting at something the user said. plain: only the words (no tool calls)."""
        msgs = self.history[-n:]
        while msgs and msgs[0]["role"] != "user":
            msgs = msgs[1:]
        if plain:
            msgs = [{"role": m["role"], "content": m["content"]} for m in msgs if m["role"] in ("user", "assistant") and m["content"].strip()]
        return msgs

    def _ask_json(self, prompt):
        """One quick, non-streamed JSON answer from the brain (memory upkeep). Same context size: no model reload."""
        return self._ask(prompt, True)

    def _ask(self, prompt, as_json=False):
        """One non-streamed answer from the brain (background work: memory, research)."""
        body = gpu.body(gpu.brain(BRAIN, FAST), {"num_ctx": CTX, "temperature": 0.1 if as_json else 0.4, "num_predict": 2500}, stream=False, think=False,
                        messages=[{"role": "user", "content": prompt}])
        if as_json:
            body["format"] = "json"
        req = urllib.request.Request(OLLAMA, json.dumps(body).encode(), {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=600) as resp:
            return json.loads(resp.read())["message"]["content"]

    def _learn(self, transcript):
        saved = self.memory.learn_from(transcript, self._ask_json)
        for fact in saved:
            print(f"   [memory] learned: {fact}")

    def _load_brain(self):
        try:
            req = urllib.request.Request(OLLAMA.replace("/chat", "/generate"), json.dumps(gpu.body(gpu.brain(BRAIN, FAST), {"num_ctx": CTX}, prompt="")).encode(),
                                         {"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=120).read()
        except Exception:                                                     # noqa: BLE001
            pass


    # ---- doing things on the PC ------------------------------------------------------------------------------
    def _agent(self, user_text):
        """Talk, or act: the brain may call tools (approved by the user where needed), see the results and carry on."""
        turn = [{"role": "user", "content": user_text}]                       # this request, with its real tool calls and results
        self.request_text = user_text
        said, did, t0, nudges, done, repeats = [], [], time.perf_counter(), 0, {}, 0
        self.allow_all = getattr(self, "allow_all_next", False)              # "yes to all" (or a routine): no cards for safe steps
        self.allow_all_next = False
        follow_up = len(user_text.split()) <= 8 and re.search(r"\b(it|that|this|them|again|do it|send|go ahead|now|too|also)\b", user_text.lower())
        tools = actions.pick_tools(user_text, getattr(self, "last_tools", ()) if follow_up else ())
        if memory_mod.from_statement(user_text) and not re.search(r"\b(play|open|put on|search|find|send|set|turn)\b", user_text, re.I):
            tools = []                                                        # "my favourite playlist is maths" is news, not an order
        self.last_tools = [t["function"]["name"] for t in tools]
        name = re.search(r"\b(?:my name is|call me|my full name is)\s+([A-Za-z][\w'-]{1,30}(?:\s+[A-Za-z][\w'-]{1,30}){0,3})", user_text, re.I)
        if name:                                                              # the one fact worth saving the moment it is said
            full = " ".join(w.capitalize() for w in name.group(1).split() if w.lower() not in ("and", "but", "so", "please", "okay"))
            self.memory.add(f"The user's name is {full}.", "name")
        facts = self.memory.relevant(user_text)                               # only what relates to this request (+ name, preferences)
        known = ("\nWhat you remember about the user (use it naturally when it helps; don't recite it unprompted):\n" +
                 "\n".join(f"- {f}" for f in facts)) if facts else ""
        math_result = actions.spoken_math(user_text)
        if math_result:                                                       # sums are computed, never guessed
            known += f"\nExact calculation (use this result): {math_result[0]} = {math_result[1]}"
        if any(t["function"]["name"] == "research" for t in tools) and RECENT.search(user_text) and not re.search(r"\b(open|play|my)\b", user_text, re.I):
            print("   [research first: the answer may be newer than the brain's knowledge]")
            query = user_text
            if not re.search(r"\b20\d\d\b", query) and re.search(r"\b(last|latest|most recent|recent|this year'?s?|newest)\b", query, re.I):
                query = re.sub(r"\b(the )?(last|latest|most recent|recent|this year'?s?|newest)\b\s*", "", query, flags=re.I).strip(" ?") + \
                        f" {time.strftime('%Y')}"                             # an event: "the last World Cup" -> "World Cup 2026"
            query = re.sub(r"\b(current(ly)?|right now|at the moment|today)\b\s*", "", query, flags=re.I).strip(" ?")   # an office or a price: no year
            found = self.hands.run("research", {"query": query})              # (falls back to Wikipedia when DuckDuckGo blocks)
            if found.startswith("Search results"):                            # in the question itself, sources first: a small model
                turn[0] = {"role": "user", "content": (                       # reads them there; in the system prompt it overruled them
                    f"Web search results from today, {time.strftime('%B %d, %Y')}:\n{found[:6000]}\n\n"
                    f"Your own knowledge is out of date. Using only those results, answer with the concrete names or numbers: {user_text}")}
            else:                                                             # an old answer would sound right and be wrong
                known += ("\nThe web search failed just now. Tell the user you couldn't check it right now; "
                          "do not answer from your own knowledge, it is out of date.")
            tools = [t for t in tools if t["function"]["name"] != "research"]
        if any(t["function"]["name"] == "system_info" for t in tools):        # time, date, battery, disks: fetched, never guessed
            known += f"\nRight now (real, use these exact values): {self.hands.run('system_info', {})}"
            tools = [t for t in tools if t["function"]["name"] != "system_info"]
        forced = False                                                        # it said "one moment" over and over: made to answer once
        for _ in range(14):                                                   # enough steps for a longer task
            if repeats >= 2:
                break
            past = self.recent(14) if tools else self.recent(14, plain=True)   # no tools: no old tool calls in sight to imitate
            messages = [{"role": "system", "content": SYSTEM + (AGENT_NOTE if tools else "") + known}] + past + turn
            body = gpu.body(gpu.brain(BRAIN, FAST), {"num_ctx": CTX, "temperature": 0.5}, stream=True, think=False, messages=messages)
            if tools:
                body["tools"] = tools                                         # only what this request asks for; plain chat gets none
            reply, calls, first_token = self._stream(body)
            if "<tool" in reply or "<|" in reply:                             # a call written as text: keep it only if this request may use it
                reply, written = self.text_tool_calls(reply)
                allowed = {t["function"]["name"] for t in tools}
                calls = calls + [c for c in written if c["function"]["name"] in allowed]
            if not re.search(r"\w", reply):                                   # a lone "?" or "." between tool calls
                reply = ""
            if reply.strip():
                said.append(reply.strip())
                print(f"   solu: {reply.strip()}   (brain first token {1000 * (first_token or 0):.0f} ms)")
            if self.cancel.is_set():
                break
            if not calls:
                # "I'll check that for you..." and then nothing: words don't do anything, so push it to act once.
                if tools and nudges < 3 and not forced and PROMISE.search(reply.lower()):     # (no tools: nothing it could call)
                    nudges += 1
                    turn += [{"role": "assistant", "content": reply},
                             {"role": "user", "content": "(Eflow: nothing has happened yet. Call the tool now, without commenting on this note.)"}]
                    continue
                if reply.strip():
                    turn.append({"role": "assistant", "content": reply.strip()})
                break
            looks = all(c.get("function", {}).get("name") in ("look_at_screen", "system_info") for c in calls)
            if reply.strip() and not PROMISE.search(reply.lower()) and (did or looks):
                # It has answered. A look it was not asked for (or one more round after acting) would only make it talk twice.
                turn.append({"role": "assistant", "content": reply.strip()})   # it already acted and has answered: extra calls are noise
                break
            turn.append({"role": "assistant", "content": reply, "tool_calls": calls})
            fresh, refused = 0, False
            for call in calls:
                name = call.get("function", {}).get("name", "")
                args = call.get("function", {}).get("arguments") or {}
                if isinstance(args, str):
                    try:
                        args = json.loads(args)
                    except ValueError:
                        args = {}
                key = name + json.dumps(args, sort_keys=True)
                if key in done:                                               # small models like to repeat a call that already worked
                    repeats += 1
                    result = f"already done a moment ago ({done[key][:200]}). Do not do it again; just answer the user."
                else:
                    result = done[key] = self.use_tool(name, args)
                    did.append(name)
                    fresh += 1
                print(f"   [tool] {name}({json.dumps(args, ensure_ascii=False)[:200]}) -> {result[:200]!r}")
                turn.append({"role": "tool", "content": result[:2000], "tool_name": name})
                if result.startswith(("the user said no", "the user did not answer")):
                    refused = True
                    break
                if self.cancel.is_set():
                    break
            if refused:                                                       # "no" ends the request: nothing else is tried
                text = "Okay, I won't." if result.startswith("the user said no") else "Okay, I'll leave it for now."
                said.append(text)
                self.say(text)
                turn.append({"role": "assistant", "content": text})
                break
            if not fresh and reply.strip():                                   # it answered and only repeated itself: the answer stands
                if PROMISE.search(reply.lower()) and not forced:              # ... unless that "answer" is only "one moment, I'm looking": make it answer now
                    forced, tools, repeats = True, [], 0
                    turn.append({"role": "user", "content": "(Eflow: you already have what the tool showed. Answer the user now in one or two plain "
                                                            "sentences. Do not call a tool and do not say you are still looking.)"})
                    continue
                break
        text = re.sub(r"(\s+[?.!,]+)+$", "", " ".join(said).strip())
        if said and said[-1] in ("Okay, I won't.", "Okay, I'll leave it for now.") and len(said) > 1:
            text = said[-1]                                                   # don't show half-plans for something that was refused
        if not text and did:                                                  # it acted but said nothing: still answer
            text = "Done."
            self.say(text)
        print(f"   (request handled in {time.perf_counter() - t0:.1f} s, {len(did)} action(s))")
        turn[0] = {"role": "user", "content": user_text}                      # the history keeps the words, not the search results
        for m in turn:                                                        # remember it, but keep old tool output short
            if m["content"].startswith("(Eflow:"):
                continue
            self.history.append(dict(m, content=m["content"][:300]) if m["role"] == "tool" else m)
        text = EMOJI.sub("", text).strip()
        self.spoken = (self.spoken + [text])[-3:]
        self.event("said", text=text)
        self._wait_for_playback()

    def use_tool(self, name, args):
        if name not in actions.TOOL_NAMES:
            return f"error: there is no tool called {name}"
        if name == "write_file" and not re.search(r"\b(file|save|note|notes|document|doc|txt|write (it|that|this) down|folder)\b",
                                                   getattr(self, "request_text", "").lower()):
            return "skipped: the user did not ask for a file. Don't make files; carry on with what they asked."   # small models love extra files
        if actions.needs_approval(name, args) and not (getattr(self, "allow_all", False) and not actions.describe(name, args)[2]):
            answer = self.ask_permission(name, args)
            if answer == "deny":
                return "the user said no. Do not do it and do not try another way; just say okay."
            if answer == "timeout":
                return "the user did not answer, so it was not done."
            if answer == "all":
                self.allow_all = True                                         # the remaining safe steps of this request go ahead
        self.event("action", tool=name)
        return self.hands.run(name, args)
