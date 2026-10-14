"""Deep research in the background: plan -> search -> read -> notes -> report (HTML), opened in Chrome when it is done.

Everything runs on this PC except the web searches (DuckDuckGo) and the page downloads. The brain is the same local model as
the conversation; each step waits while Solu is talking, so a running job never makes the conversation slow.
Reports are saved in Documents\\Solu Research (an .html to read, and an .md with the plain text).
"""
import datetime
import html
import json
import os
import re
import subprocess
import threading
import time

import actions

FOLDER = os.path.join(os.path.expanduser("~"), "Documents", "Solu Research")
DEPTH = {"deep": {"questions": 5, "pages": 3, "chars": 3500}, "standard": {"questions": 3, "pages": 2, "chars": 2500}}

PLAN = ("You are planning web research on this topic: {topic}\n"
        "Write {n} focused sub-questions that together cover it well (background, how it works or what happened, key facts and "
        "numbers, different views or problems, the latest state). For each, give a short web search query.\n"
        'Return JSON: {{"title": "a clear report title", "questions": [{{"question": "...", "query": "..."}}]}}')
NOTES = ("Research topic: {topic}\nSub-question: {question}\n\nSources:\n{sources}\n\n"
         "Using ONLY these sources, write the key findings that answer the sub-question: 4 to 8 bullet points, each a full, specific "
         "sentence with facts, names, dates or numbers where the sources give them, ending with the source number like [3]. "
         "If the sources disagree, say so. If they don't answer it, write one bullet saying what is unknown. Bullets only.")
REPORT = ("Write a research report titled \"{title}\" on: {topic}\n\nResearch notes (with source numbers):\n{notes}\n\n"
          "Write it in Markdown, for a curious reader, clear and well organised:\n"
          "## Summary\n(4-6 sentences: the main answer)\n## Key findings\n(5-8 bullets)\n"
          "then 2 to 4 sections with your own '## ' headings that explain the topic in depth (paragraphs, bullets where useful)\n"
          "## Conclusion\n(a short paragraph)\n"
          "Keep the source numbers like [3] after the facts they support. Use only facts from the notes; never invent sources or numbers. "
          "Keep dates and numbers consistent; where the notes disagree, say so instead of picking one silently. "
          "Do not add a source list (it is added automatically).")


def _slug(text):
    return re.sub(r"[^\w\- ]+", "", text).strip()[:60] or "research"


def _chrome():
    for base in (os.environ.get("PROGRAMFILES", ""), os.environ.get("PROGRAMFILES(X86)", ""), os.environ.get("LOCALAPPDATA", "")):
        p = os.path.join(base, "Google", "Chrome", "Application", "chrome.exe")
        if base and os.path.exists(p):
            return p
    return None


def open_report(path):
    """Open in a new Chrome window (or the default browser if Chrome is not installed)."""
    chrome = _chrome()
    if chrome:
        subprocess.Popen([chrome, "--new-window", "file:///" + path.replace("\\", "/")], creationflags=0x08000000)
    else:
        os.startfile(path)


def _inline(text, nsources):
    t = html.escape(text)
    t = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"(?<!\*)\*(?!\s)(.+?)(?<!\s)\*(?!\*)", r"<em>\1</em>", t)

    def cite(m):
        nums = [int(n) for n in re.findall(r"\d+", m.group(0)) if 1 <= int(n) <= nsources]
        return "".join(f'<a class="cite" href="#s{n}">{n}</a>' for n in nums) if nums else m.group(0)
    return re.sub(r"\[(\d+(?:\s*[,\-–]\s*\d+)*)\]", cite, t)


def to_html(title, topic, markdown, sources, meta):
    body, in_list = [], False
    for line in markdown.splitlines():
        s = line.strip()
        if re.match(r"^[-*•]\s+", s):
            if not in_list:
                body.append("<ul>")
                in_list = True
            item = re.sub(r"^[-*•]\s+", "", s)
            body.append(f"<li>{_inline(item, len(sources))}</li>")
            continue
        if in_list:
            body.append("</ul>")
            in_list = False
        if not s:
            continue
        h = re.match(r"^(#{1,4})\s+(.*)", s)
        if h:
            level = min(3, max(2, len(h.group(1))))
            name = h.group(2).strip()
            cls = ' class="summary-h"' if name.lower() == "summary" else ""
            body.append(f"<h{level}{cls}>{_inline(name, len(sources))}</h{level}>")
        else:
            body.append(f"<p>{_inline(s, len(sources))}</p>")
    if in_list:
        body.append("</ul>")
    items = []
    for i, (t, u) in enumerate(sources):
        site = re.sub(r"^https?://(www\.)?", "", u).split("/")[0]
        items.append(f'<li id="s{i + 1}"><a href="{html.escape(u)}">{html.escape(t or u)}</a><span>{html.escape(site)}</span></li>')
    src = "".join(items)
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(title)}</title><style>
:root{{--bg:#f7f6f3;--card:#fff;--ink:#1d1d1f;--dim:#6e6e73;--line:#e5e3df;--accent:#0a7cff;--soft:#eef5ff}}
@media (prefers-color-scheme:dark){{:root{{--bg:#111214;--card:#1b1c1f;--ink:#ececee;--dim:#9a9aa1;--line:#2c2d31;--accent:#5aa9ff;--soft:#16243a}}}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--ink);font:17px/1.65 "Segoe UI Variable Text","Segoe UI",system-ui,sans-serif}}
main{{max-width:780px;margin:0 auto;padding:56px 24px 80px}}
.kicker{{color:var(--accent);font-weight:600;font-size:13px;letter-spacing:.08em;text-transform:uppercase}}
h1{{font:700 36px/1.2 "Segoe UI Variable Display","Segoe UI",sans-serif;margin:8px 0 10px;letter-spacing:-.01em}}
.meta{{color:var(--dim);font-size:14px;margin-bottom:34px}}
h2{{font-size:23px;margin:40px 0 10px;padding-top:10px;border-top:1px solid var(--line)}}
h2.summary-h{{border:0;margin-top:0}}h3{{font-size:19px;margin:26px 0 8px}}
h2.summary-h+p,h2.summary-h+p+p{{background:var(--soft);border-radius:14px;padding:18px 20px;margin:10px 0}}
ul{{padding-left:22px}}li{{margin:6px 0}}
a.cite{{display:inline-block;min-width:18px;padding:0 5px;margin-left:2px;border-radius:9px;background:var(--soft);color:var(--accent);
font-size:12px;line-height:18px;text-align:center;text-decoration:none;vertical-align:2px}}
.sources{{margin-top:44px;border-top:1px solid var(--line);padding-top:10px}}.sources ol{{padding-left:22px;font-size:15px}}
.sources li a{{color:var(--ink);text-decoration:none}}.sources li a:hover{{color:var(--accent)}}
.sources li span{{color:var(--dim);margin-left:8px;font-size:13px}}
footer{{color:var(--dim);font-size:13px;margin-top:40px}}</style></head><body><main>
<div class="kicker">Solu research</div><h1>{html.escape(title)}</h1>
<div class="meta">{html.escape(meta)}</div>
{''.join(body)}
<section class="sources"><h2>Sources</h2><ol>{src}</ol></section>
<footer>Researched and written by Solu on this PC from {len(sources)} web sources. Check important facts in the sources.</footer>
</main></body></html>"""


class Job(threading.Thread):
    def __init__(self, topic, depth, ask, wait_idle, done):
        super().__init__(daemon=True)
        self.topic, self.depth = topic.strip().rstrip("?.!"), depth if depth in DEPTH else "deep"
        self.ask, self.wait_idle, self.done = ask, wait_idle, done      # ask(prompt, json?) -> text; wait_idle(); done(job)
        self.status, self.progress = "planning", 0.0
        self.title, self.path, self.markdown, self.summary, self.error = self.topic, None, "", "", None
        self.started = time.time()

    def brain(self, prompt, as_json=False):
        self.wait_idle()                                              # never compete with Solu talking to the user
        return self.ask(prompt, as_json)

    def run(self):
        try:
            self._run()
        except Exception as e:                                        # noqa: BLE001
            self.error = str(e)[:200]
            self.status = "failed"
            print(f"   [research] failed: {e}")
        self.done(self)

    def _run(self):
        cfg = DEPTH[self.depth]
        plan = json.loads(self.brain(PLAN.format(topic=self.topic, n=cfg["questions"]), as_json=True))
        self.title = (plan.get("title") or self.topic).strip()
        questions = [q for q in plan.get("questions", []) if q.get("query")][:cfg["questions"]] or \
            [{"question": self.topic, "query": self.topic}]
        print(f"   [research] {self.title}: {len(questions)} questions")

        # Search and read: every page is fetched once, in parallel; sources are numbered across the whole report.
        self.status, sources, per_q = "searching the web", [], []
        for q in questions:
            try:
                hits = actions.search_results(q["query"])[:cfg["pages"] + 2]
            except Exception:                                         # noqa: BLE001
                hits = []
            per_q.append(hits)
        urls = []
        for hits in per_q:
            for title, url, _ in hits:
                if url not in [u for _, u in urls]:
                    urls.append((title, url))
        texts = {}
        self.status = f"reading {len(urls)} pages"

        def fetch(u):
            texts[u] = actions._page_text(u, limit=cfg["chars"])
        workers = [threading.Thread(target=fetch, args=(u,), daemon=True) for _, u in urls]
        for w in workers:
            w.start()
        for w in workers:
            w.join(12)
        index = {}
        for title, url in urls:
            if texts.get(url):
                index[url] = len(sources) + 1
                sources.append((title, url))

        # Notes for each sub-question, from its own pages.
        notes = []
        for i, (q, hits) in enumerate(zip(questions, per_q)):
            self.status = f"taking notes ({i + 1} of {len(questions)})"
            self.progress = 0.2 + 0.5 * i / len(questions)
            block, used = [], 0
            for title, url, snippet in hits:
                if url in index and used < cfg["pages"]:
                    block.append(f"[{index[url]}] {title}\n{texts[url]}")
                    used += 1
            if not block:
                block = [f"(search snippets) {t}: {s}" for t, _, s in hits[:4]] or ["(no sources found)"]
            found = self.brain(NOTES.format(topic=self.topic, question=q.get("question", q["query"]), sources="\n\n".join(block)))
            notes.append(f"### {q.get('question', q['query'])}\n{found.strip()}")

        # The report.
        self.status, self.progress = "writing the report", 0.75
        self.markdown = self.brain(REPORT.format(title=self.title, topic=self.topic, notes="\n\n".join(notes))).strip()
        self.markdown = re.sub(r"^```(?:markdown)?|```$", "", self.markdown).strip()
        self.markdown = re.sub(r"^#\s+[^\n]*\n+", "", self.markdown).strip()          # the title is on the page already
        m = re.search(r"##\s*Summary\s*\n(.+?)(?=\n##\s|\Z)", self.markdown, re.S | re.I)
        self.summary = re.sub(r"\s*\[\d+(?:[,\-–\s]*\d+)*\]", "", (m.group(1) if m else self.markdown[:600])).strip()

        os.makedirs(FOLDER, exist_ok=True)
        stamp = datetime.datetime.now()
        base = os.path.join(FOLDER, f"{stamp:%Y-%m-%d %H%M} {_slug(self.title)}")
        minutes = max(1, round((time.time() - self.started) / 60))
        meta = f"{stamp:%d %B %Y}  ·  {'Deep' if self.depth == 'deep' else 'Standard'} research  ·  {len(sources)} sources  ·  {minutes} min"
        with open(base + ".html", "w", encoding="utf-8") as f:
            f.write(to_html(self.title, self.topic, self.markdown, sources, meta))
        with open(base + ".md", "w", encoding="utf-8") as f:
            f.write(f"# {self.title}\n\n{self.markdown}\n\n## Sources\n" + "\n".join(f"{i + 1}. {t} - {u}" for i, (t, u) in enumerate(sources)))
        self.path = base + ".html"
        self.status, self.progress = "done", 1.0
        print(f"   [research] done in {time.time() - self.started:.0f} s: {self.path}")
        open_report(self.path)


def latest_report():
    """(html path, markdown text) of the newest saved report, or (None, '')."""
    try:
        files = sorted((os.path.join(FOLDER, f) for f in os.listdir(FOLDER) if f.endswith(".md")), key=os.path.getmtime)
    except OSError:
        return None, ""
    if not files:
        return None, ""
    with open(files[-1], encoding="utf-8") as f:
        return files[-1][:-3] + ".html", f.read()
