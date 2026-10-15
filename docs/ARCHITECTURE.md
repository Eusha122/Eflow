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

**Live lyrics** (`services/LyricsService`). Time-synced lyrics come from [LRCLIB](https://lrclib.net) (free,
open, no key): one request per new song on a worker thread (WinHTTP), first `/api/get` with title,
artist, album and length (exact match), then `/api/search` picking the result with synced lyrics and
the closest length (video titles are cleaned first: "(Official Video)", "Artist - Topic", "Artist -
Title"). Every answer, a miss too, is cached in `%LOCALAPPDATA%\Eflow\lyrics\<hash>.lrc` (misses are
asked again after 7 days; offline is not remembered as a miss), so a replayed song costs nothing.
The island picks the line from the position `MediaService` already extrapolates (0.15 s early) and
sets ONE timer for the moment the next line starts; a change plays a 0.35 s rise-and-fade and then
no frames are drawn until the next line. Closed island: a slim pill (28 px) as wide as the line, with
a green note; live pills (downloads, tasks, agents) and a Solu conversation take priority. Music tab:
the line being sung and the next one between the artist and the progress bar (the tab grows 36 px
only while a song has lyrics); a long line steps down a size before it is cut. The lyrics button at
the right of the transport row switches all of it (`lyrics.json`); it is green when lyrics show and
quiet when the song has none. Clicking the lyric pill opens the Music tab.
While a pill covers the idle bar, Solu's microphone-level updates no longer request frames (the meter
is not visible): music with vocals made Solu's speech detector fire ~14 times a second, which had kept
the closed island drawing ~17 frames/s.

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
* **The live view stays out of the way.** Tabs, the round buttons and Settings always work mid-conversation (the live view
  steps aside until the conversation ends); a click anywhere outside the island tucks it away while Solu keeps talking (a
  low-level mouse hook that exists only while the live view is up); opening the island again brings the live view back. Only
  the **End** pill ends the conversation, and the view grows to fit a long answer.
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
* **Doing things on the PC** (`solu/actions.py`). Ordinary requests go through an agent loop: qwen3:8b gets 18 tools through
  Ollama tool calling (open apps via `Get-StartApps` + `shell:AppsFolder`, websites, web search, media keys, volume keys,
  timers, system info incl. free disk space, look at / click text on the screen via Windows OCR word boxes, type (SendInput
  Unicode), shortcuts, list/focus/min/max/close windows, find/open/read/write files, clipboard, PowerShell). Up to 8 steps per
  request; real tool calls and results are kept in the history; exact repeats are not executed twice; a "let me..." without a
  call gets one nudge; a refusal ends the request. **Permission:** harmless tools run at once (music, volume, timer, time,
  web search, reading the screen); everything else emits `approve` (id, action, detail, risky, always) and waits up to
  120 s. `SoluService` turns it into a `PermissionRequest` (id + `kSoluApprovalBase`, source "Solu") that `App` merges into
  the island's approval card next to Claude Code / Codex requests; Reject / Approve / **Always allow** (offered for safe
  kinds only, never for commands, file writes or closing windows) go back as `{"cmd":"approval"}`. The user can also answer
  out loud ("yes", "no", "always"): while a card is up the microphone stays on and the next sentence is the answer.
  "Always allow" is stored in `%LOCALAPPDATA%\Eflow\solu_trust.json` ("Solu, forget what I allowed" clears it). Commands
  matching destructive patterns are marked *Potentially destructive*; typing / clicking cards name the target window.
* **Long-term memory** (`solu/memory.py`, `%LOCALAPPDATA%\Eflow\solu_memory.json`). Not sentence by sentence: "remember
  that ..." and "my name is / call me ..." are saved at once; everything else is picked out once per conversation, after it
  ends, by one background JSON call to the brain (at most 3 lasting facts: name, people and how to reach them, preferences,
  plans; usually none). Duplicates update the existing fact. Recall is a word match (microseconds, no embedding model): each
  request gets the core facts (name, preferences) plus up to 5 that share words with it. Voice: "what do you remember about
  me?", "forget (about) ...", "forget everything" (a `.bak` copy is kept). Each request also only gets the tools its words
  ask for (`actions.pick_tools`); plain chat gets none, and time/date/battery/disk questions are answered from real values
  fetched before the model replies.
* **Deep research** (`solu/research.py`). "Deep research on X" / "research X in depth and make a doc" calls the
  `deep_research` tool, which starts a background job and returns at once: plan (3 or 5 sub-questions, JSON), search each
  (DuckDuckGo), fetch every page once in parallel, notes per sub-question with numbered sources, then a Markdown report
  (Summary, Key findings, 2-4 sections, Conclusion) rendered to a styled HTML page (light/dark, clickable citations, source list)
  and an .md copy in `Documents\Solu Research`, opened in a new Chrome window. Every brain call of the job waits while Solu is
  speaking, so the conversation stays quick. When done Solu says so; "read it" reads the summary, "read the whole report",
  "how's the research going?", "open my last report". A deep job takes about 2 minutes on the RTX 5050.
* **Exact answers.** Arithmetic in a request ("12 times 8", "15% of 240", "square root of 144") is computed
  (`actions.spoken_math`, a safe AST evaluator) and handed to the model, like the time and disk values.
* **Live activities on the thin bar** (`core/Activity.h`, `services/ActivityMonitor`, `App::PushActivities`,
  `IslandWindow::DrawLive`). When something is going on, the idle bar grows into a 352 x 32 pill (icon, title, detail,
  percentage / countdown ring / close button, a thin progress line, "+N" when more is waiting). Sources: browser downloads
  (`.crdownload` / `.part` growing in Downloads), Explorer copy dialogs (`OperationStatusWindow`, "34% complete"), Solu's research
  and note taking (`activity` / `activity_end` events), the focus timer, Solu's nudges, and 5-second "done" flashes (which make the
  cat cheer). Order: done, nudges, focus, Solu's jobs, downloads/copies. Hovering a pill opens the island after 0.8 s, so a click
  (open) or the close button can come first. Repaints only once a second (50 ms while an indeterminate shimmer runs).
* **Focus timer.** `focus` events from Solu ("start a 25 minute focus session") run a Pomodoro in `App` (focus / break rounds);
  the pill shows the countdown ring, Solu says when a round or break ends, the cat naps during breaks.
* **The cat's day** (`Mascot::Mood`, `Mascot::Cheer`). Sleepy (eyes closed, slow breathing, "z"s) from 23:00 to 05:00 and during
  focus breaks; worried (wide eyes, a sweat drop) after a minute of >90% CPU or >94% memory; a happy hop with sparkles when a
  download, copy, research, notes or a focus round finishes. Checked every 30 s with `GetSystemTimes` (no extra sampling).
* **Privacy key** Ctrl+Alt+H: every island is hidden and Solu's microphone closes (`pause`); press again to bring both back.
* **Quick capture** Ctrl+Alt+Q (or "Solu, let me show you" / "translate this part"): `CaptureOverlay` dims all screens, the
  dragged area stays clear (colour-keyed hole, white outline), and Solu gets `region` to explain or translate just that area.
* **Solu's skills** (`solu/skills.py`): clipboard history (in memory only; passwords and keys hidden), routines ("when I say
  study mode, ..."; `solu_routines.json`; their safe steps run without cards), tidy a folder (plan on a card, never overwrites),
  study buddy (quiz from the screen, a PDF or a topic; spoken answers graded; weak points remembered), lecture / meeting notes
  (the PC's sound through WASAPI loopback, Whisper every 30 s, notes page in Documents\Solu Notes), morning briefing (weather
  from wttr.in, notification senders shared by Eflow, plans from memory, a headline), rare nudges (low battery, a plan for
  tomorrow, two hours without a break; at most one per 30 min, six a day; shown as pills, never spoken), smart messaging
  (WhatsApp: search, open, check the chat header, card, send; email: memory or Gmail's contact suggestion, card, send),
  translate and explain-the-error answers for the screen.
* **Agents tab** (`services/AgentMonitor`, `IslandWindow::DrawAgents`). Replaces "AI Requests". Two logo buttons (Claude Code,
  Codex) with a status dot; it opens on whichever agent is working (else the most recent) until you pick one. Monitoring is
  read-only and needs no hooks: `ReadDirectoryChangesW`-style notifications (`FindFirstChangeNotification`, recursive) on
  `~/.claude/projects` and `~/.codex/sessions` wake a worker that reads only the appended bytes of each session file from the
  last 6 hours and parses Claude's `user` / `assistant` / `tool_use` / `tool_result` lines and Codex's `event_msg` /
  `response_item` lines into a per-session feed (what you asked, what it said, each tool with its subject, "Finished"),
  a status (working / idle; "waiting for you" when an approval card from that tool is pending) and a title. Giant lines
  (tool output > 300 KB) are not parsed. Per session: project, title, feed with the mouse wheel / Up / Down. While an agent is
  working it also shows as a live pill on the thin bar; when it finishes there is a pill and the cat cheers.
  **Prompting** works like typing to the agent in your editor: the box (Ctrl+Alt+A opens it focused; a "+" button starts a
  new chat) runs `claude -p <text> --resume <id> --output-format json` / `codex exec resume <id> <text>` in the session's folder,
  hidden, stdin = NUL. For Claude the island's `PermissionRequest` hook is attached for that run only (`--settings`, a file in
  `%LOCALAPPDATA%\Eflow`) unless it is already connected globally, so approvals arrive as cards. The reply shows up in the same
  feed because it lands in the session's log. The island takes keyboard focus only while you type (`WS_EX_NOACTIVATE` is lifted,
  `WM_CHAR` / `WM_KEYDOWN` edit the text; Esc or clicking elsewhere gives focus back). No IME: type with your keyboard layout
  (Unicode characters work), paste with Ctrl+V. Limit: your already-open editor panel does not show the prompt live; the work
  and the answer do appear here and in the session file, and the editor shows them after a reload.
  **Go to the window:** clicking the session header (project name with a small "open" glyph) calls `AgentMonitor::FocusHost`: it finds
  the VS Code / Cursor window whose title has the project's name (or a terminal titled claude / codex; otherwise it runs
  `code <folder>`) and brings it to the front. Ctrl+click also opens that Claude conversation in the Claude panel through the
  extension's `vscode://anthropic.claude-code/open?session=<id>` link (reloads it, so prompts sent from the island show up;
  VS Code asks once to allow the link). A pill about an agent opens the island straight on this tab.
  Solu: `agent_status` ("what is Claude doing?") and `tell_agent` ("tell Codex to ...", approved on a card) use the same data.
* **The file shelf** (`services/ShelfStore`, `IslandWindow::DrawShelf`, tab 6 "Shelf"). The island is an OLE drop target
  (`RegisterDragDrop`, `OleInitialize` in `App`): dragging files over it, even when it is the thin bar, opens it on the Shelf
  tab with a big breathing "+" drop zone; releasing holds the files **by reference** (nothing is copied or moved), saved in
  `%LOCALAPPDATA%\Eflow\shelf.json`, missing files are dropped on load. Each item gets a real thumbnail or its icon from
  `IShellItemImageFactory` on a worker thread. Cards pop in, lift on hover, have an x (take off the shelf; the file is untouched),
  click selects (Ctrl for several), double-click opens, right-click shows it in Explorer, the "+" card (or the empty zone)
  opens a file picker, the wheel slides along the shelf, "Clear" empties it. **Dragging out:** pressing a card and pulling
  starts a genuine shell drag (`SHDoDragDrop` with an `IShellItemArray` data object: file list, IDList, names, contents), for
  all selected cards (or just that one). Only copy / link are offered, so the originals never move. The island stays open
  during the drag, ignores its own drag coming back, and lets go afterwards.
* **Long tasks** (`solu/tasks.py`, `solu/uia.py`; `%LOCALAPPDATA%\Eflow\solu_tasks.json`). "Start a task: ...", "I want you to work on
  ...", "your goal is ...", "every morning at 8 ...", or the chat brain's `start_task` tool for a goal with many steps. One worker
  thread runs one task at a time:
  1. **Plan**: one JSON model call writes 3-9 concrete steps (each with a "done when"), coarse (coding = one step delegated to Claude
     Code / Codex and waited for with `agent_wait`). The plan is shown as **one approval card**; once approved, non-risky actions of
     the task run without cards. Sending email/messages, prompts to agents, tidying folders, risky commands and overwriting files
     always ask separately.
  2. **Each step**, a loop of: look (UI Automation controls + one OCR pass, cached 4 s and shared with click-finding), one model call
     with a small fixed-shape prompt (goal, plan with the current step, notes, last 6 actions, the screen, a hint) and the tools
     the step needs, ONE tool call, execute, wait 0.45 s, look again and tell the model **what changed** (or that nothing did).
     `step_done(proof)` is checked once by a lenient verifier (false only when the screen contradicts the claim). A step that fails
     is retried differently, the remaining plan is revised once or twice, and then Solu asks the user (skip / try again / stop).
  3. **Safety**: actions only go to the window the task chose (the first one must be named in the goal; a changed foreground window
     refuses clicks/typing until the model brings the right one back); stuck detection by "same action and same screen"; a budget of
     90 actions / 120 minutes after which it asks to carry on.
  4. **Questions park the task**: when a task asks the user something (its own question, "I'm stuck", "carry on?") it is saved as
     *waiting* and the worker moves on to other tasks; the answer resumes it where it stopped. Results of non-screen tools
     (research, files, command output, an agent's reply) are shown to the model in full on its next turn.
  5. **Bursts**: a turn may carry up to 4 actions the model is sure of (click a field, type, Enter). They run back to back
     (0.12 s apart), each still through the window guard and approvals; the burst stops at the first failure and the model is
     told which actions did not run; the screen is read once at the end. The hand is fast: a pointer move is ~25 ms through
     8 points (apps still get hover / drag-over), a click ~60 ms, a drag ~0.35 s, typing is one SendInput batch.
  6. **Windows a task may touch**: closing, minimizing, moving or resizing only works on windows the task opened itself or the
     user's goal names; closing always asks. (A test task once closed the user's YouTube window because its title was on screen.)
  **Graphics memory** (`solu/gpu.py`): every model request goes through it. The brain is let go after 3 minutes unused (and when
  Solu quits); when it loads, 1.5 GB of the card stays free for the user's apps (Chrome, VS Code, WhatsApp crash when the card is
  full) and only the layers that fit go to the GPU, the rest to the CPU. The chosen layer count is shared between processes in
  `%LOCALAPPDATA%\Eflow\gpu_layers.json` (a different count makes Ollama reload the whole model). Speed: 53 tokens/s whole on
  the card, ~11-19 tokens/s when only 22-30 of 37 layers fit.
  The model never competes with the conversation: every model call waits while Solu or the user speaks (`chat_busy`) and is
  dropped when the user starts talking. Prompts stay about 1.5k tokens, with the same `num_ctx` (no reload). Speed on the RTX 5050:
  about 3 s of model time + 1.5 s of looking per action. Tasks are saved, a task running when Solu closes comes back *paused*.
  Voice: "pause / continue / stop the task", "how's the task going", "what tasks do you have", "clear finished tasks", answering a
  question the task asked, "every <day> at <time> <goal>" (schedules, checked every 20 s), "what's scheduled". The island shows a live
  pill ("Task", "Needs you"), and the Solu AI tab lists tasks with a progress line and pause / resume / stop buttons.
  New tools for chat and tasks: `look`, `click_element` (UI Automation first, OCR text second; `times` for repeats), `drag`,
  `wait`, `wait_for_text`, `agent_wait`, `start_task`.
* **Not in this version.** A native C++ engine instead of Python.

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
