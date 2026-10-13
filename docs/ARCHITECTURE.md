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
| 5 | Audio-reactive mascot, light theme, autostart, polish | |
| 6 | WinUI 3 settings app (optional), installer | |

## 5. Music

`services/MediaService` watches the system media sessions on its own MTA worker thread. It is
event-driven (session, media-properties, playback and timeline events, plus a 30 s safety resync);
the UI never blocks on WinRT. It prefers the system's current session unless another one is actually
playing, extrapolates the position locally between the app's sparse timeline updates, decodes artwork
with WIC (max 192 px), and posts `WM_APP_MEDIA` with an immutable snapshot. Transport buttons send
commands back to the worker; the play/pause icon flips immediately and the service confirms.

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

## 7. Mascot pipeline

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
