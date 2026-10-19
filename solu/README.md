# Solu

Solu is Eflow's voice assistant: a small cat-fox friend you talk to. Everything runs on your own PC.

* **Hears** you with Whisper (speech to text) and Silero (knows when you start and stop talking).
* **Thinks** with a local language model through [Ollama](https://ollama.com) (`qwen3:8b` by default).
* **Speaks** with Kokoro, in one of five pet voices (`presets.json`).

Eflow starts and stops Solu for you: switch **Solu AI** on in Settings (the gear on the island) and just say
**"Solu"** ("Solu, what's up?", "Hey Solu, turn on live captions"). **Ctrl+Alt+Space** also starts and ends a
conversation.

**Using the PC.** Solu has tools (`actions.py`): apps, websites, search, music, volume, timers, screen reading and clicking,
typing, shortcuts, windows, files, clipboard and PowerShell. Anything beyond the harmless ones asks first on the island's
approval card (or say "yes" / "no" / "always"). "Solu, forget what I allowed" resets the Always-allow list.

**Deep research.** "Solu, do a deep research on ..." works in the background and opens a full report (summary,
findings, sections, sources) in Chrome when it's done; reports are kept in Documents\Solu Research. "Read it" reads the summary.

**Memory.** Solu remembers lasting things about you (your name, people and how you reach them, preferences, plans),
not every sentence. Say "remember that ...", ask "what do you remember about me?", or "forget ..." / "forget everything".
It is all in `%LOCALAPPDATA%\Eflow\solu_memory.json`.

**Screen help.** "Solu, what does this error mean?" / "Solu, look at this" / "explain this code": Solu captures the window you are
in (only then), reads its text with Windows OCR and answers; picture questions use the `qwen3-vl:4b` vision model (pull it with
`ollama pull qwen3-vl:4b`; it takes 15-30 s because it swaps with the brain on an 8 GB GPU). Nothing is saved; password-manager windows are
refused; "turn off screen help" or the Settings switch disables it.

**Privacy.** While Solu is on, the microphone is open and each sentence is transcribed on your PC to spot the name
"Solu". Sentences that are not for Solu are discarded at once: nothing is stored, nothing leaves the machine. Switch
Solu off (or say "go to sleep") and the microphone closes.

## Set it up

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup_solu.ps1
```

That creates `solu\venv`, downloads the models into `solu\models` and pulls the language model. Both folders
are ignored by git. Eflow looks for Solu in `solu\` next to the repository, in a `solu\` folder next to
`eflow.exe`, or in the folder named by the `EFLOW_SOLU_DIR` environment variable.

## How a request flows

```
microphone ─► listening.py ─► "Solu, play maths" ─► Solu.handle(text)
                (VAD, Whisper, wake word)                   │
                                                            ├─ intents.py: a fixed command matches ─► do it, say so
                                                            └─ otherwise brain.py: the language model answers,
                                                               calling tools from actions.py when it needs them
```

Everything that changes the PC goes through `Solu.use_tool`, which shows an approval card (`approvals.py`) when the
action needs one. Fixed commands exist because a small model is unreliable with actions: it would say "Media paused."
without pausing anything. The brain handles everything else: conversation, questions, multi-step requests.

## Files

`Solu` (in `solu_chat.py`) is one object; its behaviour is split into mixins, one module per job.

| File | What it is |
|---|---|
| `solu_service.py` | The hidden helper Eflow runs: one JSON event per line out, one JSON command per line in. |
| `solu_chat.py` | Builds `Solu` from the mixins below; also runs on its own from a console (`run_solu.bat`). |
| `listening.py` | The microphone loop: turn-taking, the wake word, its own echo, conversations. |
| `audio.py` | Devices (following the island and Windows' default), speech-to-text, the speaker queue. |
| `intents.py` | Fixed commands: music, open, click, settings, memory, tasks, reports. **Add commands here.** |
| `brain.py` | The language model: streamed answers and the tool-using agent loop. |
| `approvals.py` | Approval cards on the island, answered by tap or by voice. |
| `screen_help.py` | Questions about the screen: OCR text first, the vision model for pictures. |
| `jobs.py` | Deep research in the background; hooks that long tasks use. |
| `skill_runner.py` | Routines, the study quiz, lecture notes, the morning briefing, messages. |
| `config.py`, `prompts.py` | Models and the model server; what the model is told. |
| `actions.py` | The tools themselves (`Hands`): apps, websites, Spotify, keyboard, mouse, files, ... |
| `tasks.py` | Long background tasks: plan, approve once, then step by step. |
| `skills.py`, `research.py`, `memory.py`, `screen.py`, `uia.py`, `gpu.py` | The building blocks the modules above use. |
| `voice_presets.py`, `presets.json` | The five voices (blends of Kokoro voices, pitch and pace). |
| `dev/` | Scratch benchmarks and tests (ignored by git). |

## Adding a command

Write one function in the right section of `intents.py`:

```python
@intent
def weather(solu, req):
    """'What's the weather?'"""
    if not re.search(r"\bweather\b", req.t):        # req.t: lower case, words only
        return None                                  # not mine: the next intent (or the brain) gets it
    return act(solu, req, "weather", {}, lambda result: result)   # use a tool, then say something
```

Intents are tried top to bottom and the first match wins, so keep patterns strict: a false match is worse than a
miss, because a miss still reaches the brain.

## Testing

`tests/hardcore_test.py` sends about 60 typed requests through `Solu.handle` (the same path as speech) and checks which
tool was used, with which arguments, and what Solu said. Tools that would change the PC are recorded instead of run;
memory and tasks use temporary files. Run all of it, or one group:

```powershell
solu\venv\Scripts\python.exe solu\tests\hardcore_test.py
solu\venv\Scripts\python.exe solu\tests\hardcore_test.py music
```

## Licences to know about

Kokoro is Apache-2.0. Its text-to-phoneme step uses espeak-ng (`espeakng-loader`), which is **GPL-3**:
decide how to ship that before distributing a bundle that includes it.
