# Solu

Solu is Eflow's voice assistant: a small cat-fox friend you talk to. Everything runs on your own PC.

* **Hears** you with Whisper (speech to text) and Silero (knows when you start and stop talking).
* **Thinks** with a local language model through [Ollama](https://ollama.com) (`qwen3:8b` by default).
* **Speaks** with Kokoro, in one of five pet voices (`presets.json`).

Eflow starts and stops Solu for you: switch **Solu AI** on in Settings (the gear on the island) and just say
**"Solu"** ("Solu, what's up?", "Hey Solu, turn on live captions"). **Ctrl+Alt+Space** also starts and ends a
conversation.

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

## Files

| File | What it is |
|---|---|
| `solu_service.py` | The hidden helper Eflow runs: one JSON event per line out, one JSON command per line in. |
| `solu_chat.py` | The conversation engine (microphone, turn taking, Whisper, the brain, Kokoro). Also runs on its own: `run_solu.bat`. |
| `voice_presets.py`, `presets.json` | The five voices (blends of Kokoro voices, pitch and pace). |
| `dev/` | Scratch benchmarks and tests (ignored by git). |

## Licences to know about

Kokoro is Apache-2.0. Its text-to-phoneme step uses espeak-ng (`espeakng-loader`), which is **GPL-3**:
decide how to ship that before distributing a bundle that includes it.
