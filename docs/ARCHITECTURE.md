# Eflow — Feasibility & Architecture

Eflow is a native Windows "dynamic island": a thin bar at the top-centre of each monitor that
expands into a control centre (music, AI permission centre, notifications).

## 1. Verdict

**Yes, it can be built, and natively.** Three areas need caveats:

| Area | Verdict | Notes |
|---|---|---|
| Island window (thin bar, hover-expand, springs, multi-monitor, always-on-top, DPI) | **Fully feasible** | Pure Win32 + DirectComposition + Direct2D/DirectWrite |
| Music: Spotify, YouTube, browser media | **Fully feasible** | One API covers all: Windows SMTC (`GlobalSystemMediaTransportControlsSessionManager`). Chromium/Edge/Firefox and Spotify publish to it. Controls work only where the app exposes the capability (e.g. YouTube has no "previous" outside a playlist). |
| AI permission centre: Claude Code | **Fully feasible, officially supported** | `PermissionRequest` hook (command hook, stdin JSON, stdout JSON decision). Also fires for the VS Code extension. |
| AI permission centre: OpenAI Codex | **Feasible, officially supported** | Codex has an equivalent `PermissionRequest` hook with the same output schema. Hooks must be trusted by the user (`/hooks`). VS Code extension support is undocumented. |
| AI permission centre: Copilot Chat / other VS Code AI extensions | **Not feasible** | Their confirmation dialogs are private to the extension. No external approval API. We do not screen-scrape or inject clicks — that would bypass security boundaries. Their *notifications* can still appear via the notification feed. |
| Windows notifications feed | **Feasible with a workaround** | `UserNotificationListener` is the only supported API. It needs **package identity** (sparse MSIX package) and user consent. Unpackaged exe → access denied. |
| Quick actions / reply on notifications | **Limited** | The listener can read and dismiss; it cannot invoke the toast's buttons. Per-app "open app" is feasible. |
| Per-app notification mute | **Feasible** | Filter in our own UI; we cannot suppress the system toast itself (only the user can, in Settings). |
| Mascot | **Done (asset)** | 2.5D puppet rig built from generated art: one skinned image mesh (8 bones) plus swappable face patches, exported as glTF (`assets/mascot/eflow.glb`). The app plays it live; see §5. |
| Audio-reactive mascot | **Feasible** | WASAPI loopback peak meter (`IAudioMeterInformation`, cheap, no capture). Phase 5. |

## 2. Why not WinUI 3 for the island itself

The brief lists WinUI 3 as *preferred*. For this specific window it is the wrong tool:

* WinUI 3 windows cannot be truly per-pixel transparent with custom hit-testing and a spring-driven
  rounded shape; you fight the system backdrop and the XAML island host.
* ~80–150 MB private working set per process, plus Windows App SDK runtime dependency, versus ~10–20 MB.
* The XAML compiler/MSBuild toolchain is not headless-friendly.
* Frame-exact animation (drive width/height/radius/opacity from one spring solver, present on vblank)
  is simplest when we own the swap chain.

So: **Win32 window + DirectComposition + Direct2D + DirectWrite for the island**. C++/WinRT is still
used (without XAML) for SMTC and the notification listener. A WinUI 3 **Settings window** (separate,
launched on demand, not resident) is the right place for WinUI — Phase 6, optional.

## 3. Components

```
eflow.exe  (resident, ~one UI thread + small workers)
 ├─ platform/   MonitorManager, FullscreenGuard, Autostart (HKCU Run), DPI
 ├─ render/     D3D11 device, DComp visual, D2D context, Theme, Spring, Easing
 ├─ island/     IslandWindow (hit-testing, hover state machine), IslandController
 │              (states: Collapsed / Peek / Expanded / Alert), Layout, Sections
 ├─ ui/         MusicView, NotificationsView, PermissionView, Mascot, controls
 ├─ services/   MediaService (SMTC), NotificationService (listener), PermissionServer
 │              (named pipe), Store (SQLite), SecretStore (DPAPI/Credential Manager)
 └─ core/       Event bus (marshals worker → UI thread via PostMessage), Settings

eflow-hook.exe  (tiny, launched by Claude Code / Codex per request)
   stdin JSON → \\.\pipe\eflow-permission-<user-SID> → blocks until decision → stdout JSON
```

### Permission flow (security model)

1. Tool runs `eflow-hook.exe` as a `PermissionRequest` hook.
2. Hook connects to the pipe (ACL: current user only) and sends the request.
3. The island shows: requesting app, action/command, working directory/project, Approve / Reject.
4. The decision returns to the hook, which prints the official `hookSpecificOutput.decision` JSON.
5. **Fail-safe rules:** never auto-approve. If Eflow is not running, the hook exits 0 with no
   output, so the tool falls back to its **own normal prompt**. If the user ignores it, a
   timeout also falls back to the native prompt (never "allow"). "Always ask" is the default; there is
   no "remember allow" in v1. Commands are shown in full (no truncation of the part that matters), with
   dangerous patterns highlighted.

### Rendering/animation

* Popup with `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE`.
* Swap chain for composition, premultiplied alpha, so real per-pixel transparency.
* The window has a fixed size (the expanded footprint) and never moves or resizes while animating.
  A window region limits its clickable area to the idle bar plus a small hover zone, so everything
  else passes clicks through to the windows beneath; the region opens up only while the panel is out.
* Fullscreen guard is event-driven: the shell's `ABN_FULLSCREENAPP` app-bar notification (an app bar
  registered without a position reserves no space) plus foreground/minimize WinEvents.
* Frames render **only while something is animating** (spring not at rest). Idle CPU ≈ 0.
* One critically-damped / under-damped spring per animated scalar (width, height, corner radius,
  content opacity), integrated analytically for stability at any frame time.
* Hover state machine with intent delay (open ~80 ms), grace period on leave (~400 ms), and pinned
  while a button is pressed, a menu is open, or a permission request is pending.

### Persistence

SQLite (amalgamation, compiled in): notification history, per-app rules, settings. Secrets (future
Spotify Web API tokens, etc.) go through DPAPI/Credential Manager, never SQLite.

## 4. Phases

| # | Phase | Status |
|---|---|---|
| 0 | Mascot asset: art, rig, expressions, clips, export pipeline | done |
| 1 | Island window: collapsed bar, hover expand/collapse, springs, multi-monitor, DPI, fullscreen guard, tray exit | done |
| 1b | Mascot player: glTF loader, GPU skinning, clip/face state machine, blink, overlay effects | done |
| 2 | Music: SMTC, artwork, progress, controls, mascot states | done |
| 3 | Permission centre: pipe server, hook exe, Claude Code + Codex connect switches (tray) | done |
| 4 | Notifications: sparse package identity, listener, list with dismiss / clear all | done |
| 4b | Notifications: history (SQLite), per-app mute, privacy | |
| 4c | System tab: CPU / GPU / memory / network rings with flip-through readings | done |
| 4d | Messages tab: chat-app (WhatsApp) notifications split out, panel resizes to fit, open-the-app action | done |
| 5 | Audio-reactive mascot, light theme, autostart, polish | |
| 6 | WinUI 3 settings app (optional), installer | |

## 5. Music

`services/MediaService` watches the system media sessions on its own MTA worker thread. It is
event-driven (session, media-properties, playback and timeline events, plus a 30 s safety resync);
the UI never blocks on WinRT. It prefers the system's current session unless another one is actually
playing, extrapolates the position locally between the app's sparse timeline updates, decodes artwork
with WIC (max 192 px), and posts `WM_APP_MEDIA` with an immutable snapshot. Transport buttons send
commands back to the worker; the play/pause icon flips immediately and the service confirms.

The progress bar is a scrubber when the app allows it (`PlaybackControls.IsPlaybackPositionEnabled`,
e.g. Spotify and browsers, but not live streams): click or drag it, the handle and time follow the
pointer (the pointer is captured, so the drag may leave the bar), and on release the worker calls
`TryChangePlaybackPositionAsync`. The new position is shown immediately; the app confirms shortly after.
A session that has just appeared can briefly report no playback info; the worker waits and retries.

The mascot's mood is layered so sources never fight: ambient (idle / music playing) < pending AI
permission request < one-shot notification reaction.

## 6. AI permission centre

* `eflow-hook.exe` is the `PermissionRequest` command hook for Claude Code and Codex (both use the same
  output schema). It forwards the hook's stdin JSON over `\.\pipe\Eflow.Permission.<user SID>`
  and prints `allow` / `deny` decisions; for anything else it prints nothing, so the tool shows its own
  prompt. That covers: Eflow not running (exits in ~20 ms), the pipe served by anything other than the
  `eflow.exe` next to the hook (checked with `GetNamedPipeServerProcessId`), "Answer in <tool>",
  and no answer within 290 s (we install the hook with a 300 s timeout).
* `PermissionServer` creates the pipe with a protected DACL for the current user only,
  `FILE_FLAG_FIRST_PIPE_INSTANCE` and `PIPE_REJECT_REMOTE_CLIENTS`; one thread per pending request.
  If the tool stops waiting (answered elsewhere, cancelled, killed), the card disappears.
* Requests are parsed with Windows.Data.Json; the card shows source, action, project and the exact
  command / path / patched files, with a "Potentially destructive" warning for risky patterns (a hint
  only: nothing is ever approved or blocked automatically).
* The tray menu's "Approve AI requests from" switches add or remove only Eflow's entry in
  `~/.claude/settings.json` or `~/.codex/hooks.json`, through an order-preserving JSON editor
  (`core/Json`), with a one-time `.eflow-backup` and an atomic replace. Codex additionally requires
  the user to trust the hook with `/hooks`.
* Not possible: Copilot Chat and other VS Code AI extensions expose no external approval API.

## 7. System tab

Four rings (CPU, GPU, Memory, Network). Clicking a ring, or scrolling the wheel over it, flips it to its
next reading with a cross-fade and the ring animating to the new value (CPU: usage, temperature, fan;
GPU: usage, temperature, fan, video memory; Memory: in use, available; Network: download, upload).
Colour stays accent-blue until a reading gets warm (amber) or hot (red).

`SystemMonitor` samples once a second on its own thread, **only while a System tab is on screen**
(reference-counted `Watch(+1/-1)` from the islands); with the tab closed nothing is sampled.

| Reading | Source | Needs |
|---|---|---|
| CPU usage, clock | PDH `Processor Information` (Task Manager's "utility" figure), kernel times as fallback | nothing |
| Memory | `GlobalMemoryStatusEx` | nothing |
| Network | `GetIfTable2` byte counters, hardware adapters only (no double counting of VPN/virtual adapters) | nothing |
| GPU usage, temperature, fan, VRAM | NVML (`nvml.dll`, loaded from System32 only); other GPUs: PDH `GPU Engine` counters (usage only) | NVIDIA driver |
| CPU temperature, CPU/case fans | optional bridge: WMI namespace `ROOT\LibreHardwareMonitor` (or OpenHardwareMonitor) | that tool running as administrator |

Windows offers no supported way to read CPU temperature or motherboard fan speed from a normal
program; that takes a hardware driver and administrator rights, which Eflow deliberately does not
include. When no sensor tool is running, those two readings say "Needs sensor app" instead of guessing.

## 8b. Microphone, speaker and camera

Not a tab: three round buttons (microphone, speaker, camera) sit at the right end of the tab row. Each has a connection light (green = the
device in use is plugged in, red = it is not, amber = still looking) and a ring that breathes with the live
level. Clicking one makes **the island itself grow** (the height spring, 176 DIP up to 296 DIP) and shows
the device menu inside it: the tab's content fades out, the menu rises in. It has a compact header (glyph,
title, "Connected", live level bars), "Automatic" and every device as a two-line row (product name over
device kind), a checkmark on the one in use, hairline separators inset from the text, and a "Sound
settings..." footer. Clicking the other button swaps the menu in place; picking a device only moves the checkmark
(the menu stays open so you can press Test straight away); clicking anywhere else, switching tabs or leaving
the island folds it back. The wheel scrolls past four rows.

* `services/AudioDevices` (worker thread, MTA) enumerates the active endpoints with `IMMDeviceEnumerator`
  and rebuilds the list from `IMMNotificationClient` events, so plugging something in or out updates the
  buttons without polling. The peak level (`IAudioMeterInformation`) is sampled ~15 times a second, only
  while the island is open (refcounted `Watch`, like the System tab). Microphone levels only move while
  something (Solu) is capturing from that device; speaker levels follow whatever is playing.
* **Picking a device makes it the Windows default** (all three roles), so Spotify, Chrome, Discord... really
  follow it; the checkmark is simply the device Windows is using, and the menu has no separate "Automatic"
  entry. Windows has no public API for this; `AudioDevices` uses `IPolicyConfig::SetDefaultEndpoint`, the same
  interface Windows' own Sound settings and every sound-switcher tool use (unchanged since Vista). The old
  Solu-only choice in `voice.json` is therefore always empty ("follow the system default"). Unplugged devices
  (e.g. a headphone jack with nothing in it) are not listed.
* **Where is it playing?** The Music tab shows a chip (headphones / speakers / monitor / headset glyph and name)
  with the output the playing app really uses. It is found from Windows' audio sessions: the output endpoint
  that has an active stream from the media app (so a per-app output choice is respected), else the default
  output. The glyph turns green while music plays there; clicking the chip opens the speaker menu.
* **Status light = "can I use it right now?"** Grey (the normal look) means not available: the device is
  missing, or it is a phone-based device whose phone is not connected. Green means connected and ready. Amber
  means in use right now (a microphone with an active capture stream, a camera that an app has open); the
  menu names the app. Windows alone cannot tell a phone webcam that is streaming from one that is not (the
  virtual camera and microphone are listed as present, and healthy, either way), so
  `services/DeviceReadiness` checks the well-known bridges (Iriun, DroidCam, EpocCam, Camo, OBS virtual
  camera): the PC app must be running and, for the phone apps, it must hold an established TCP connection
  (that is when a phone is streaming to it; checked once a second while the island is open). Devices not
  recognised as a bridge count as ready as soon as Windows lists them.
* **Test.** A small pill next to the microphone / speaker title. The microphone test opens a WASAPI capture
  stream on the chosen device for up to 5 s and only looks at the loudness (nothing is recorded or kept; it
  only ever starts from the button, and Windows' own microphone indicator shows it); the header turns into a
  scrolling live waveform and ends with a verdict ("Microphone works", "Very quiet", "No sound heard",
  "Muted", "Blocked in Windows privacy" when the microphone privacy switch is off). The speaker test plays a
  quiet two-note chime through the chosen output (and tells you if it is muted). The verdict replaces the
  status text for six seconds.
* **Mute.** Every menu has an Apple-style switch in its header (on = sound / camera on). For the microphone
  and speaker it sets the *endpoint mute* (`IAudioEndpointVolume::SetMute`, the same mute as Windows' volume
  panel, applied on the audio worker thread and read back every sample so a hardware mute key shows up too).
  A muted button gets a red slash. Right-clicking a button toggles the same switch without opening the menu.
* **Camera.** `services/CameraService` lists cameras with SetupAPI (`KSCATEGORY_VIDEO_CAMERA`, so a phone used
  as a webcam appears too), reports which apps are using one right now (the `LastUsedTimeStart/Stop` stamps
  under `CapabilityAccessManager\ConsentStore\webcam`; a desktop app whose stamp never closed is ignored
  unless its program is actually running) and offers the switch. Windows has no way for a normal program to
  mute a webcam (disabling the device needs administrator rights), so the camera switch flips Windows' own
  per-user privacy setting, `Value` = `Deny`/`Allow` under `ConsentStore\webcam` and `...\NonPackaged`
  (Settings > Privacy > Camera): no app can open the camera until it is switched back. Read once a second
  while the island is open.
* The tab strip is left-aligned and its padding adapts so five labels and the three buttons fit 600 DIP.

## 8c. Solu, the voice assistant, inside the island

Solu is a Python program (`solu/`: Whisper for hearing, a local Ollama model for thinking, Kokoro for the
voice) that Eflow runs as a **hidden helper process**. Nothing is loaded until the user switches it on.

* **Process.** `services/SoluService` starts `solu/venv/Scripts/python.exe solu_service.py` with no window, inside
  a job object (it dies with Eflow, even if Eflow crashes), and talks to it over its stdin/stdout, one JSON line
  each way. Events: `state` (loading, ready, listening, thinking, speaking), `heard`, `said`, `level` (how loud Solu
  is, for the cat's mouth), `mic` (how loud somebody is talking, for the thin bar), `setting` (Solu changed one of
  its own settings by voice) and `error`. Commands: `talk`, `stop`, `voice`, `headphones`, `ask`, `quit`. Its log is
  `%LOCALAPPDATA%\Eflow\solu.log`; the settings (on, voice, headphones, captions) are in `solu.json` there. If Solu was
  on it wakes up with Eflow.
* **No clicking: it listens for its name.** While Solu is on, the microphone is open and every spoken sentence is
  transcribed locally (Whisper, nothing stored or sent anywhere). A sentence that does not address Solu is thrown
  away. "Solu", "Hey Solu, ...", "..., Solu" (and the ways Whisper mishears the name: Salu, Zulu, so low...) start a
  conversation and carry on with whatever followed the name: "Solu, what's up?" is answered at once, a bare "Solu"
  is answered with "Yes?". A conversation ends by itself after 15 s of quiet, or with "bye" / the hotkey
  (**Ctrl+Alt+Space**, also a click on the conversation view); then it goes back to waiting for its name.
* **Settings by voice.** Before anything goes to the language model, a sentence is checked against a small set of
  controls Solu handles itself: "turn on/off live captions", "headphones mode on/off", "use the tiny voice",
  "screen help on/off", "go to sleep" (switches itself off), "stop", "bye". It does the
  thing, says so, and reports a `setting` event so Eflow's menu follows.
* **Live captions** is a setting. On: a conversation opens the island and shows the live view (what you said, the
  answer, the microphone). Off: the island stays closed and you just talk; the thin bar shows Solu is there.
* **Speakers or headphones.** Speaker mode (default): Solu does not listen while it talks (plus a second after), and
  it drops anything that is just its own words coming back through the microphone. Headphones mode: you can talk
  over it.
* **UI.** The **Solu AI** tab: status (with a dot on the tab label), what to say, and the last exchange. The **gear** is
  general Settings (a menu that will grow): Solu AI on/off, Live captions, Headphones mode, Voice (a sub-page with the
  five voices; picking one makes Solu say hello in it). The **thin bar** shows five tiny green dots while Solu is
  awake and waiting, and they dance with the voice (yours, then Solu's); amber dots chase each other while it
  thinks. It costs no frames while nothing is said.
* **The cat.** `Mascot::SetTalk`: wide attentive eyes while listening, three pulsing dots while thinking, and while
  speaking the rhythmic bob with the mouth opening and closing with the voice level.
* **Screen help** (`solu/screen.py`). "Solu, what does this error mean?", "Solu, look at this", "explain this code": only
  when asked, Solu captures the *foreground window* (`PrintWindow` with `PW_RENDERFULLCONTENT`, so nothing in front of
  it leaks in; the desktop if no window is active or you say "my whole screen"), reads it with Windows' built-in OCR
  (~0.5 s) and gives the text plus the window title to the brain (qwen3:8b, first word in ~3 s). Questions about pictures,
  or windows with almost no text, use the vision model qwen3-vl:4b instead (it swaps in and out of the 8 GB GPU, so 15-30 s;
  Solu says "One moment" first and reloads the brain afterwards). Nothing is stored: the capture lives for one question.
  A `looking` event carries the window title and a 176 px thumbnail, which the conversation view shows ("Looking at ...",
  with a scanning line while Solu reads) so it is never secret what was seen. Windows whose title or program contains
  password / authenticator / key-vault words are refused. The **Screen help** switch in Settings (or "turn off screen help")
  disables it; the helper then starts with `--no-screen`.
* **Not in this version.** Running tasks on the PC and memory; a native C++ engine instead of Python.

## 9. Mascot pipeline

The character art was generated as five aligned images on a transparent background
(`art/source/`: master + happy, surprised, wink, blink). `tools/build_mascot.ps1` turns them into
the runtime asset:

1. `tools/matte.py`: normalise alpha, bring everything to a 2048 px canvas.
2. `tools/face_patches.py`: cut feathered eye/brow and mouth patches from each expression.
3. Blender 5.2.2, headless (`tools/blender/pipeline.py`):
   - `build_rig.py`: grid mesh over the master (empty cells dropped), 8 bones
     (root, head, earL, earR, antenna, pawL, pawR, tail) with procedurally computed weights, and
     the face patches as quads skinned 100% to `head`.
   - `build_anim.py`: clips `idle` (3 s loop), `music` (1 s loop), `notify` (0.9 s one-shot),
     `permission` (2.4 s loop), keyed from small motion functions.
   - `export_glb.py`: `assets/mascot/eflow.glb`.
4. Preview renders (EEVEE, unlit) and `build/mascot/render/preview.html`.

In the app the mascot lives inside the open panel (right-hand side), anchored to the live panel rect so
it rides the open/close springs and rises in with the content. It renders only while the panel is
visible: a closed, idle island renders no frames at all. Notifications and AI permission requests open
the panel on their own (a notification closes again after a few seconds; a permission request stays
until it is answered) on the matching tab.

Runtime contract for the app's player:

* Draw `eflow_base`, then the face patch nodes for the current expression
  (`face_<expression>_{eyeL,eyeR,mouth}`); neutral = no patches. All materials are alpha-blended.
* Clip → default expression: idle → neutral (with periodic `blink`), music → happy,
  notify → surprised, permission → wink.
* Notes, "?" and badge effects are drawn by the app, not baked into the asset.

Running Blender operators through the live editor's MCP socket crashed Blender twice, so the
pipeline only ever runs Blender headless.

## Notifications

`UserNotificationListener` refuses processes without package identity, so Eflow ships a **sparse package**
(`package/AppxManifest.xml`: `runFullTrust` + `userNotificationListener`, `AllowExternalContent`). It is
registered with `tools/register_package.ps1 [debug|release]` (needs Developer Mode; an installer will do the
same with a signed package). The identity is only granted to a process *started through the package*, so
`eflow.exe` relaunches itself once via `IApplicationActivationManager` (`core/PackageIdentity`) when a
package registered for its own folder exists; otherwise it runs normally and the Notifications tab says the
feature is unavailable.

`services/NotificationService` mirrors the action center on a worker thread: event-driven
(`NotificationChanged`, with a 20 s resync as a safety net), newest first, counts arrivals so the island
opens on the Notifications tab (and the mascot reacts) only for notifications that appear after startup.
Dismiss removes one notification from the action center by id; "Clear all" removes everything, so it is
only ever called from the explicit button. Classic desktop apps (Chrome, ...) expose no logo, so rows get a
tinted initial tile; packaged apps use their real logo. Notification text is never written to the log.
