# Eflow

A native Windows "dynamic island": a thin bar at the top of the screen that expands into a small
control centre for music, notifications and AI coding-tool permission requests, with an animated
companion that reacts to what is happening.

<p align="center"><img src="docs/media/mascot.webp" width="360" alt="Eflow mascot reacting to music"></p>

> **Status: early development.** The island window (idle bar, hover-to-open panel, tabs, multi-monitor,
> fullscreen auto-hide, tray icon), the animated mascot inside it and the Music tab (any app using
> Windows media sessions: Spotify, YouTube and other sites in Chrome/Edge/Firefox, Media Player...) are
> done, and so are AI permission requests from Claude Code and Codex, the Windows notification list and
> the System tab (CPU, GPU, memory and network rings; click or scroll a ring for its next reading).

### The Messages tab

WhatsApp message notifications get their own tab (sender, text, time). It grows and shrinks to fit what
is there. Windows does not allow one app to type into another app's notification, so there is no reply
box; each message has **Open in WhatsApp** instead. To route another chat app there, add its name to
`kChatApps` in `src/island/IslandWindow.cpp`.

### The System tab

CPU and GPU usage, memory and internet speed come straight from Windows; GPU temperature, fan and video
memory from NVIDIA's driver library. Windows itself cannot report CPU temperature or CPU/case-fan speed
to normal programs (that needs a hardware driver and administrator rights), so those two readings say
"Needs sensor app" unless the free [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
is running as administrator, in which case Eflow reads them from it. Nothing is sampled unless the System
tab is on screen.

### Approving AI requests in the island

Right-click the tray icon, then **Approve AI requests from** and pick Claude Code and/or Codex. Eflow
adds one hook entry to that tool's user settings (a backup is kept). From then on the tool's
permission prompts appear in the island with **Approve**, **Reject** and **Answer in <tool>**. If Eflow
is closed, or you don't answer within about five minutes, the tool simply asks in its own window;
nothing is ever approved automatically. Codex asks you to trust new hooks once: run `/hooks` in Codex.

## Planned features

- **Island**: nearly invisible when idle, smooth spring-animated expansion on hover, every monitor,
  high-DPI aware, hides during fullscreen apps, starts with Windows.
- **Music**: Spotify, YouTube and browser media through Windows' media session API; artwork,
  progress and playback controls.
- **AI permission centre**: approve or reject Claude Code and OpenAI Codex permission requests from
  the island, via each tool's official `PermissionRequest` hook. Nothing is ever auto-approved, and
  if Eflow isn't running the tool falls back to its own prompt.
- **Notifications**: incoming Windows notifications with history, per-app muting and privacy
  options.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the feasibility study, the design and the
phase plan.

## Technology

C++20, Win32, DirectComposition, Direct2D/DirectWrite and C++/WinRT. The mascot is a rigged glTF
asset built with Blender and played live by the app.

## Repository layout

```
art/source/        generated character art (master + 4 expressions), the mascot's inputs
assets/mascot/     eflow.glb, the rigged, animated mascot loaded by the app
docs/              architecture and design notes
src/               the island app (C++)
tools/             mascot build pipeline (Python + headless Blender)
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

## Rebuilding the mascot

Only needed after changing the art, rig or animations (`tools/blender/build_anim.py` holds the
motion). Requires Windows and Python 3.11+.

```powershell
tools\setup.ps1          # once: Python packages + portable Blender 5.2.2 (checksum-verified)
tools\build_mascot.ps1   # rebuild assets/mascot/eflow.glb and the previews
```

Previews land in `build/mascot/render/`. Open `preview.html` there to see every animation on light
and dark backgrounds.
