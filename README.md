# Eflow

A native Windows "dynamic island": a thin bar at the top of the screen that expands into a small
control centre for music, notifications and AI coding-tool permission requests, with an animated
companion that reacts to what is happening.

<p align="center"><img src="docs/media/mascot.webp" width="360" alt="Eflow mascot reacting to music"></p>

> **Status: early development.** The mascot asset is finished. The island app itself is being
> built next, and `CMakeLists.txt` does not produce a working executable yet.

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

## Rebuilding the mascot

Only needed after changing the art, rig or animations (`tools/blender/build_anim.py` holds the
motion). Requires Windows and Python 3.11+.

```powershell
tools\setup.ps1          # once: Python packages + portable Blender 5.2.2 (checksum-verified)
tools\build_mascot.ps1   # rebuild assets/mascot/eflow.glb and the previews
```

Previews land in `build/mascot/render/`. Open `preview.html` there to see every animation on light
and dark backgrounds.
