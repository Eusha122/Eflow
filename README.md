# Eflow

A native Windows "dynamic island": a thin bar at the top of the screen that expands into a small
control centre for music, notifications and AI coding-tool permission requests, with an animated
companion that reacts to what is happening.

<p align="center"><img src="docs/media/mascot.webp" width="360" alt="Eflow mascot reacting to music"></p>

> **Status: in development, not released yet.** The island (a thin bar that clicks pass straight through; a middle-click
> or the scroll wheel opens it), the animated cat inside it, Music with live lyrics, notifications with per-app muting,
> one-click login codes, Claude Code and Codex (their steps, a prompt box and approvals), the System tab, Solu, the
> Shelf, Reminders, sending files with SafeShare, the phone link, device and screenshot pills are all working.

### The Music tab

Shuffle and repeat buttons flank the transport controls (green when on). A chip under the title shows where the
sound comes out (headphones, speakers, monitor...); click it to switch.

### The System tab

CPU and GPU usage, memory and internet speed come straight from Windows; GPU temperature, fan and video
memory from NVIDIA's driver library. Windows itself cannot report CPU temperature or CPU/case-fan speed
to normal programs (that needs a hardware driver and administrator rights), so those two readings say
"Needs sensor app" unless the free [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
is running as administrator, in which case Eflow reads them from it. Nothing is sampled unless the System
tab is on screen.

### Microphone, speaker and camera

Three round buttons at the right end of the tab row, each with a status light: **green** = connected and
ready, **amber** = in use right now (the menu says by which app), **grey** = not available, so connect it
first. A phone used as a webcam or microphone (Iriun, DroidCam, ...) stays grey until the phone is really
connected to the PC app. Click a button and the island grows to show a menu: a switch to **mute** it (for
the camera: **block** it), a **Test** button (the microphone shows a live waveform and tells you whether it
hears you; the speaker plays a short chime), and the devices Windows knows. Right-click a button to mute or
unmute it without opening the menu. Picking a microphone or speaker does not change your Windows default;
the choice is saved in `%LOCALAPPDATA%\Eflow\voice.json` for the voice assistant to follow. Windows cannot
mute a webcam from a normal program, so the camera switch turns off Windows' own camera privacy setting
(Settings > Privacy > Camera) for your user; turn it back on from the same switch.

### Solu, your voice assistant

Run `tools\setup_solu.ps1` once, then switch **Solu AI** on in Settings (the gear). From then on just say
**"Solu"**: "Solu, what's up?", "Hey Solu, turn on live captions", "Solu, use the tiny voice". It carries on from what you
said; Ctrl+Alt+Space also starts and ends a conversation. It hears you with Whisper, thinks with a local model through
Ollama and answers in a pet voice you choose; the cat on the island listens, thinks and talks with it. A small
indicator on the thin bar shows Solu is awake and moves with the voices. Everything runs on your PC: while Solu is on,
what the microphone hears is transcribed locally to spot its name, and sentences that are not for Solu are thrown away.
**Live captions** (a setting Solu can also change by voice) decides whether a conversation opens the island or you just
talk. Using speakers? Solu waits until it has finished talking before it listens; wearing headphones? Switch on
*Headphones mode* and you can talk over it.

**Solu can use your PC:** *"Solu, open Spotify"*, *"close Chrome"*, *"set a 10 minute timer"*, *"what's taking space on my C drive?"*,
*"click Sign in"*, *"type my address"*, *"run ipconfig"*. Harmless things (music, volume, timers, the time, a web search) just happen;
anything else shows an **Approve / Reject / Always allow** card on the island first, and you can also simply say *yes* or *no*.
It runs a local model, so it is quick and dependable for single actions and simpler tasks, and less reliable for long multi-step jobs.

**Live on the island:** downloads and file copies show as live pills on the thin bar, so does Solu's research, a
focus timer ("Solu, start a focus session") and the odd polite nudge. **Ctrl+Alt+Q** lets you drag over any part of the
screen for Solu to explain or translate; **Ctrl+Alt+H** hides everything and closes Solu's microphone (press again to come
back). Also try: "good morning", "quiz me on this page", "take notes of this lecture", "clean up my downloads",
"what did I copy earlier?", "when I say study mode, ...", "tell Shoily I'll be late".

**Long tasks:** give Solu a whole goal ("Solu, start a task: research X, write it up and email it to me", "work on tidying my
downloads and report back", "every morning at 8 open my notes"). It writes a short plan, you approve it once on the island, and
then it works through it by itself in the background: looking at the screen, clicking, typing, searching, writing, even handing
coding to Claude Code or Codex. It checks each step, tries another way when something fails, asks you when it is stuck, and never
slows the conversation. Say "pause the task", "how's the task going", "stop the task", or use the buttons in the Solu AI tab.

**Shelf:** drag files onto the island (it opens into two zones: Shelf and SafeShare) to hold them, then drag them out again into
any folder, app or web page, one or several at a time. Double-click opens a file, right-click shows it in its folder.

**Agents:** the island's *Agents* tab shows what Claude Code and Codex are doing (click a logo to switch; it opens on
the busy one), lets you type to them from the island (Ctrl+Alt+A) and shows their approval requests as cards. Or ask Solu:
*"what is Claude doing?"*, *"tell Codex to fix the build"*.

**Screen help:** say *"Solu, what does this error mean?"* or *"Solu, look at this"* and it looks at the window you are in, reads it
and answers out loud, so you never need a screenshot. It only looks when you ask, shows a small picture of what it saw, stores nothing,
refuses password-manager style windows, and can be switched off in Settings. See `solu\README.md`.

### Approving AI requests in the island

Right-click the tray icon, then **Approve AI requests from** and pick Claude Code and/or Codex. Eflow
adds one hook entry to that tool's user settings (a backup is kept). From then on the tool's
permission prompts appear in the island with **Approve**, **Reject** and **Answer in <tool>**. If Eflow
is closed, or you don't answer within about five minutes, the tool simply asks in its own window;
nothing is ever approved automatically. Codex asks you to trust new hooks once: run `/hooks` in Codex.

## Still to come

- An installer (signed MSIX) and a settings window.
- Solu ignoring sung vocals, so it never answers a song.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the feasibility study, the design and the
phase plan.

## Technology

C++20, Win32, DirectComposition, Direct2D/DirectWrite and C++/WinRT. The mascot is a rigged glTF
asset built with Blender and played live by the app.

## Repository layout

```
art/source/        generated character art: master + 4 expressions (the mascot's inputs) and extra poses
assets/branding/   the logo: app icon (and a bolder drawing for 16-24 px), marks for light/dark/one colour
assets/mascot/     eflow.glb, the rigged, animated mascot loaded by the app
docs/              architecture and design notes
package/           the sparse package that gives Eflow an identity for Windows notifications
solu/              Solu, the voice assistant (Python)
src/               the island app (C++); src/eflow.ico is made from assets/branding
tests/             unit tests (eflow-tests.exe)
third_party/       cgltf, qrcodegen
tools/             build scripts, the mascot pipeline (Python + headless Blender) and end-to-end tests
website/           the product page, safeshare.co/eflow (React + Vite)
```

## Building the app

Requires Windows 10/11 and Visual Studio 2022 or newer with the "Desktop development with C++"
workload (it provides the compiler, CMake and Ninja).

```powershell
tools\build_app.ps1            # debug build   -> build\debug\eflow.exe
tools\build_app.ps1 release    # release build -> build\release\eflow.exe
```

Eflow lives in the tray: right-click its icon for "Start with Windows" and "Quit Eflow". Only
packaged builds (`-DEFLOW_PACKAGED=ON`, for the installer) turn on "Start with Windows" the first time
they run; local builds never change it.

## The website

`website/` is Eflow's product page: the home page and a tour where a cursor acts out every feature on a live replica
of the island. It is its own project (needs a current Node.js, e.g. 22 LTS), served under `/eflow/`.

```powershell
cd website
npm install
npm run dev      # http://127.0.0.1:5174/eflow/
npm run build    # type-checks, then builds into website/dist/eflow
```

Any static host can serve `website/dist`; on Vercel, `website/vercel.json` already sets it up. To keep the address
**safeshare.co/eflow**, the SafeShare project then forwards that path with one rewrite in its `vercel.json`:
`{ "source": "/eflow/:path*", "destination": "https://<the Eflow site's address>/eflow/:path*" }`.

## Rebuilding the mascot

Only needed after changing the art, rig or animations (`tools/blender/build_anim.py` holds the
motion). Requires Windows and Python 3.11+.

```powershell
tools\setup.ps1          # once: Python packages + portable Blender 5.2.2 (checksum-verified)
tools\build_mascot.ps1   # rebuild assets/mascot/eflow.glb and the previews
```

Previews land in `build/mascot/render/`. Open `preview.html` there to see every animation on light
and dark backgrounds.
