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
| Mascot | **Feasible** | Vector drawn in D2D (no assets) with a state machine; swappable for sprite sheets later. |
| Audio-reactive mascot | **Feasible** | WASAPI loopback peak meter (`IAudioMeterInformation`, cheap, no capture). Phase 4. |

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

* Layered popup, `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE`.
* Swap chain for composition, premultiplied alpha, so real per-pixel transparency.
* Window is sized to the island's *current* bounds when idle (collapsed bar + small hot zone), so it never
  blocks clicks on windows beneath. Expanded size only while open.
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
| 1 | Island window: collapsed bar, hover expand/collapse, springs, multi-monitor, DPI, fullscreen guard, tray exit | this repo |
| 2 | Music: SMTC, artwork, progress, controls, mascot states | |
| 3 | Permission centre: pipe server, hook exe, Claude Code + Codex installers (`eflow-hook --install`) | |
| 4 | Notifications: sparse package identity, listener, history (SQLite), per-app/privacy | |
| 5 | Audio-reactive mascot, light theme, autostart, polish | |
| 6 | WinUI 3 settings app (optional), installer | |
