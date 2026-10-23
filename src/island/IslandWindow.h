#pragma once
#include <windows.h>

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include "core/Gfx.h"
#include "island/Spring.h"
#include "mascot/Mascot.h"
#include "services/AudioDevices.h"
#include "services/CameraService.h"
#include "services/LyricsService.h"
#include "services/MediaService.h"
#include "services/NotificationMutes.h"
#include "services/NotificationService.h"
#include "services/PermissionServer.h"
#include "core/Activity.h"
#include "core/Firewall.h"
#include "services/SoluSetup.h"
#include "core/When.h"
#include "island/TextField.h"
#include "services/AgentMonitor.h"
#include "services/ReminderStore.h"
#include "services/SafeShareTypes.h"
#include "services/ShelfStore.h"

#include <oleidl.h>
#include <set>
#include <unordered_map>
#include "services/SoluService.h"
#include "services/SystemMonitor.h"

namespace eflow {

class MascotRenderer;

inline constexpr int kSectionCount = 8;   // Music, Notifications, Solu AI, Agents, System, Shelf, Reminders, SafeShare
inline constexpr int kMetricCount = 4;    // CPU, GPU, Memory, Network

// One island per monitor: a borderless, always-on-top, non-activating popup whose pixels come from a
// DirectComposition swap chain (true per-pixel alpha). The window never moves or resizes while
// animating; a window region keeps everything outside the idle bar click-through. The mascot lives
// inside the open panel and only renders while the panel is visible.
class IslandWindow {
public:
    enum class Section { Music, Notifications, Solu, Ai, System, Shelf, Reminders, SafeShare };
    // The welcome tour's steps (TourView.cpp); public only so StartTour's caller can jump straight to one.
    enum class TourStep { Invite, Hello, Glance, Notifications, Agents, Files, Solu, Done, Count };

    // `owner` receives WM_APP_DEVICE_LOST when the GPU device goes away. `mascotAsset`/`mascotRenderer`
    // may be null, in which case the island runs without the mascot.
    IslandWindow(Gfx& gfx, HMONITOR monitor, HWND owner, std::shared_ptr<const MascotAsset> mascotAsset,
                 MascotRenderer* mascotRenderer);
    ~IslandWindow();
    IslandWindow(const IslandWindow&) = delete;
    IslandWindow& operator=(const IslandWindow&) = delete;

    bool Create();
    HMONITOR monitor() const { return monitor_; }
    HWND hwnd() const { return hwnd_; }
    void SetSuppressed(bool suppressed);   // hidden while a fullscreen app owns this monitor
    void SetMascotState(Mascot::State state);    // test/debug entry point mapping onto the setters below
    void SetMusicPlaying(bool playing);          // ambient mood; never opens or closes the panel
    void OnNotification(int count);              // `count` new notifications: opens briefly on the Notifications tab
    void SetPermissionPending(bool pending);     // opens on the AI tab and stays open until resolved
    void SetMedia(const MediaInfo& media);
    void SetLyrics(const Lyrics& lyrics);
    void SetLyricsOn(bool on);
    // Settings > Closed island: may it show the song's lyrics, and Claude Code / Codex at work. `handler(lyrics, agents)`
    // is called when the user changes them.
    void SetGlimpsePrefs(bool lyrics, bool agents);
    void SetGlimpseHandler(std::function<void(bool, bool)> handler) { glimpseHandler_ = std::move(handler); }
    void SetLyricsHandler(std::function<void(bool)> handler) { lyricsHandler_ = std::move(handler); }
    void SetMediaHandler(std::function<void(MediaCommand, double)> handler) { mediaHandler_ = std::move(handler); }
    void SetNotifications(const NotificationSnapshot& snapshot);
    void SetNotificationHandlers(std::function<void(uint32_t)> dismiss, std::function<void()> clearAll) {
        dismissHandler_ = std::move(dismiss);
        clearAllHandler_ = std::move(clearAll);
    }
    // Settings > Notifications: which apps may open the island (muted ones stay in the list, quietly).
    void SetNotificationMutes(const NotificationMutes& mutes);
    void SetMuteHandlers(std::function<void(const std::wstring&, const std::wstring&, bool)> app, std::function<void(bool)> all) {
        muteApp_ = std::move(app);
        muteAll_ = std::move(all);
    }
    void SetSystem(const SystemSnapshot& snapshot);
    void SetAudio(const AudioSnapshot& snapshot);
    // `watch(true)` while the island is open (live levels on the buttons); `select(isInput, id)` when the user picks a
    // device (empty id = follow the system default).
    void SetAudioHandlers(std::function<void(bool)> watch, std::function<void(bool, const std::wstring&)> select,
                          std::function<void(int, bool)> mute, std::function<void(bool, bool)> test) {
        audioWatchHandler_ = std::move(watch);
        audioSelectHandler_ = std::move(select);
        muteHandler_ = std::move(mute);
        audioTestHandler_ = std::move(test);
    }
    void SetCamera(const CameraSnapshot& snapshot);
    void SetSolu(const SoluSnapshot& snapshot);
    void ShowForSolu();
    // The Agents tab: Claude Code and Codex sessions, what they are doing, and a box to send them a prompt.
    void SetAgents(const AgentSnapshot& snapshot);
    void SetAgentHandlers(std::function<bool(AgentTool, const std::wstring&, const std::wstring&, const std::wstring&, bool, std::wstring&)> send,
                          std::function<void(AgentTool)> connect) {
        agentSend_ = std::move(send);
        agentConnect_ = std::move(connect);
    }
    // The file shelf: drop files on the island to hold them, drag them out again into any folder, app or web page.
    void SetShelf(const ShelfSnapshot& snapshot);
    void SetShelfHandlers(std::function<void(const std::vector<std::wstring>&)> add, std::function<void(const std::wstring&)> remove,
                          std::function<void()> clear) {
        shelfAdd_ = std::move(add);
        shelfRemove_ = std::move(remove);
        shelfClear_ = std::move(clear);
    }
    // Reminders and alarms: the list, a box to type a new one ("Codeforces contest 4 March 8pm"), and the card that shows
    // while one rings (the island opens for it). The view is in RemindersView.cpp.
    void SetReminders(const ReminderSnapshot& snapshot);
    void SetReminderHandlers(std::function<void(const std::wstring&, const std::wstring&, int64_t, Repeat, int)> add,
                             std::function<void(uint32_t)> remove, std::function<void(uint32_t)> dismiss,
                             std::function<void(uint32_t, int)> snooze) {
        reminderAdd_ = std::move(add);
        reminderRemove_ = std::move(remove);
        reminderDismiss_ = std::move(dismiss);
        reminderSnooze_ = std::move(snooze);
    }
    // SafeShare: drop files on the tab, then send them by email (an encrypted SafeShare room) or to a phone on the same
    // Wi-Fi. The view is in SafeShareView.cpp; the work is done by the app's SafeShare and phone-link services.
    struct SafeShareHandlers {
        std::function<void(const ShareSendRequest&)> send;
        std::function<void()> cancel;                            // stop the send in progress
        std::function<void()> done;                              // the result was seen: back to an empty form
        std::function<bool(const std::wstring&)> copyLink;       // a sent room's link (with its key) to the clipboard
        std::function<void(const std::wstring&)> remove;         // delete a sent room now
        std::function<void(bool)> phone;                         // start / stop the phone link
        std::function<void()> phoneAnyway;                       // start it on a network Windows calls public (the user's word)
        std::function<void(bool)> phoneAllow;                    // the phone that asked to connect: allow / deny
        std::function<void(const std::vector<std::wstring>&)> offer;   // the files the phone may download
    };
    void SetSafeShare(const SafeShareSnapshot& snapshot);
    void SetSafeShareHandlers(SafeShareHandlers handlers) { share_ = std::move(handlers); }
    // Files dragged onto the island (called by its OLE drop target): it opens into two zones, the Shelf and SafeShare, and
    // the files go to the one they are dropped on. `screen`: the pointer, in screen pixels.
    DWORD FilesDragEnter(IDataObject* data, POINT screen);
    DWORD FilesDragOver(POINT screen);
    void FilesDragLeave();
    DWORD FilesDrop(IDataObject* data, POINT screen);
    void SetTaskHandler(std::function<void(const std::string&, const std::string&)> handler) { taskHandler_ = std::move(handler); }
    void SetAgentOpen(std::function<void(const AgentSession&, bool)> open) { agentOpen_ = std::move(open); }
    void FocusAgentPrompt();                     // Ctrl+Alt+A: open on the Agents tab with the cursor in the prompt box
    // Live pills on the thin bar (downloads, copies, focus timer, research, nudges), most important first.
    void SetActivities(const std::vector<Activity>& items);
    void SetActivityDismiss(std::function<void(const std::wstring&)> dismiss) { activityDismiss_ = std::move(dismiss); }
    void SetMarkHandler(std::function<void(const Activity&)> mark) { mark_ = std::move(mark); }   // a pill's eject / save mark was pressed
    void SetMood(Mascot::Mood mood) { if (mascot_) mascot_->SetMood(mood); }
    void Cheer() { if (mascot_) { mascot_->Cheer(); Animate(); } }                          // the hotkey was pressed: open so the conversation is visible
    void SetSoluHandlers(std::function<void(bool)> enable, std::function<void()> talk, std::function<void(const std::wstring&)> voice,
                         std::function<void(bool)> headphones, std::function<void()> stop, std::function<void(bool)> captions,
                         std::function<void(bool)> screen) {
        soluEnable_ = std::move(enable);
        soluTalk_ = std::move(talk);
        soluVoice_ = std::move(voice);
        soluHeadphones_ = std::move(headphones);
        soluStop_ = std::move(stop);
        soluCaptions_ = std::move(captions);
        soluScreen_ = std::move(screen);
    }
#ifndef NDEBUG
    void DebugOpenTab(int section);              // scripted tests: open on a tab and keep it open (-1 releases)
    std::string DebugLayout() const;             // the shoot director: where the buttons are, in screen pixels (JSON)
    void DebugSnapshot(const std::wstring& path);   // the next frame's pixels as a PNG (layout checks; never the screen)
    void DebugSettings(int page);                // the gear's menu open on `page` (0 options .. 3 closed island); -1 closes
    void DebugDropPreview(int zone);             // as if files hovered over zone 0 (Shelf) / 1 (SafeShare); -1 ends it
#endif
    // Called with true while the System tab is on screen (so the monitor samples), false otherwise.
    void SetSystemWatchHandler(std::function<void(bool)> handler) { systemWatchHandler_ = std::move(handler); }
    void SetPermissions(const std::vector<PermissionRequest>& pending);
    void SetPermissionHandler(std::function<void(uint64_t, PermissionDecision)> handler) {
        permissionHandler_ = std::move(handler);
    }
    // The welcome tour (TourView.cpp): the closed island invites the user to open it (that is the first lesson), then the
    // panel walks through what Eflow does, with an animated scene for each step, and asks for each permission where it
    // is explained. The app runs it once, and again from the tray.
    struct TourStatus {                          // what the steps show about this PC (the rest comes from the snapshots)
        FirewallAccess firewall = FirewallAccess::Unknown;
        bool micBlocked = false;
        bool autostart = false;
        bool claudeAvailable = false, codexAvailable = false;   // used on this PC (so there is something to connect)
    };
    struct TourHandlers {
        std::function<void()> allowNotifications, connectAgents, allowPhone, turnOnSolu;
        std::function<void(const std::wstring&)> soluSetupBegin;   // the recommended model's id
        std::function<void(const wchar_t*)> openSettings;    // an ms-settings: page
        std::function<void(bool)> autostart;
        std::function<void(bool)> finished;                  // true: completed, false: skipped
    };
    void StartTour(TourStep startAt = TourStep::Hello);   // startAt lets "Set up Solu" on the Solu tab jump straight there
    void StopTour();
    void SetSoluSetup(bool detected, const SoluSetupPlan& plan, const SoluSetupProgress& progress);
    bool TourRunning() const { return tourOn_; }
    void SetTourStatus(const TourStatus& status);
    void SetTourHandlers(TourHandlers handlers) { tour_ = std::move(handlers); }
#ifndef NDEBUG
    void DebugTour(int step);                    // layout checks: the tour open on `step` (-1 ends it)
#endif

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    // geometry (DIPs unless noted)
    float TopInset() const;
    float ContentAlpha() const;                  // 0 (closed) .. 1 (open): panel content and mascot opacity
    bool Pinned() const;                         // something needs the panel to stay open
    D2D1_RECT_F IslandRect() const;              // island in window-client DIPs
    std::array<D2D1_RECT_F, kSectionCount> TabRects(const D2D1_RECT_F& island) const;
    D2D1_POINT_2F MascotPoint(float x, float y) const;   // mascot model space -> window DIPs (inside the panel)
    void Place();                                // size/position the HWND for the monitor + DPI
    void UpdateScale();
    D2D1_POINT_2F ToDip(POINT clientPx) const { return {clientPx.x / scale_, clientPx.y / scale_}; }
    bool CursorInIsland(float slackDip) const;
    void SetHitRegion(bool full);                // clickable shape: whole window, or just the idle bar
    // Closed, the island is click-through (every click reaches the app below; a middle click opens it, see MouseHook.h).
    // Only while a mouse button is held does the closed bar take the pointer, so files dragged onto it can be dropped.
    void ApplyClickThrough();
    void UpdateMouseHook();                      // tell the hook where the closed island is
    bool holding_ = false;                       // a button has been held for a moment: maybe files are being carried
    ULONGLONG wheelOpenedAt_ = 0;                // when a wheel turn opened the island

    // state machine
    void Expand();
    void Collapse();
    void OnMouseMove(D2D1_POINT_2F p);
    int HitTest(D2D1_POINT_2F p) const;          // -1 = none, 0..2 = tabs, kHitPrev.. = media controls
    void OnClick(int id);
    // Something wants a tab (a notification, an approval card, a ringing reminder). While the user is using the island
    // (pointer on it, a menu open, typing) it never switches under them: the tab gets a dot instead, and a card that needs
    // them (important) is shown as soon as they let go. Returns true when it switched now.
    bool UserBusy() const;
    bool Present(Section section, bool important);
    void ShowPending();                          // the user let go: show the card that waited
    int pendingSection_ = -1;                    // a card waiting for the user to finish (-1: none)
    std::array<int, kSectionCount> tabDot_{};    // per tab: 0 nothing, 1 something new, 2 something needs you
    int notifBadge_ = 0;                         // "+3" on the Notifications tab: arrived while the user was on another tab

    // animation + rendering
    void Animate();                              // queue a frame now (vsync-paced while the panel moves)
    void Frame();
    bool StepAnimation(float dt);                // true while the panel/tabs are still moving
    void OnSettled();
    void Render();
    bool CreateSurface();
    void DrawShadow(ID2D1DeviceContext* dc, D2D1_ROUNDED_RECT shape, float progress);
    void DrawIsland(ID2D1DeviceContext* dc, float progress);
    void DrawContent(ID2D1DeviceContext* dc, D2D1_RECT_F island, float alpha);
    void DrawMusic(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    std::array<D2D1_POINT_2F, 3> ControlCenters(const D2D1_RECT_F& island) const;   // previous, play/pause, next
    std::array<D2D1_POINT_2F, 2> ExtraControlCenters(const D2D1_RECT_F& island) const;   // shuffle, repeat
    struct OutputInfo {                          // where the playing app's sound comes out
        bool valid = false, playingHere = false;
        const wchar_t* glyph = L"\uE7F5";
        std::wstring label;
    };
    OutputInfo MediaOutput() const;
    D2D1_RECT_F OutputChipRect(const D2D1_RECT_F& base, const OutputInfo& out) const;
    void DrawNotifications(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    D2D1_RECT_F NotificationList(const D2D1_RECT_F& island) const;                 // clip rect of the rows
    D2D1_RECT_F NotificationRow(const D2D1_RECT_F& island, size_t index) const;    // row `index` at the current scroll
    D2D1_RECT_F NotificationClose(const D2D1_RECT_F& row) const;
    D2D1_RECT_F NotificationClear(const D2D1_RECT_F& island) const;
    void DrawSystem(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    void DrawRing(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float radius, float thickness, float fraction,
                  D2D1_COLOR_F color, float alpha, float trackAlpha);
    D2D1_RECT_F RingCell(const D2D1_RECT_F& panel, int index) const;
    void CyclePage(int metric, int direction);
    void UpdateSystemWatch();                    // start/stop sampling to match what is on screen
    // Microphone / speaker: two round buttons on the tab row; each makes the island grow to show a device menu.
    struct PopoverLayout {
        D2D1_RECT_F card{}, footer{};            // the menu, and its last row
        float rowsTop = 0;                       // first device row
        int shown = 0, total = 0;                // rows visible / devices + "Automatic"
    };
    D2D1_RECT_F AudioButton(const D2D1_RECT_F& island, int side) const;   // side 0 = microphone, 1 = speaker, 2 = camera
    bool SideValid(int side) const { return side == 2 ? camera_.valid : side == 3 ? true : audio_.valid; }
    bool SideMuted(int side) const {             // for Solu "muted" means switched off
        return side == 0 ? audio_.input.muted : side == 1 ? audio_.output.muted : side == 2 ? camera_.blocked : !solu_.enabled;
    }
    enum class Dot { Unavailable, Ready, InUse };   // grey: not available / green: connected and ready / amber: in use now
    Dot SideDot(int side) const;
    std::wstring SideInUse(int side) const;      // "Discord, Chrome"
    std::wstring SideHint(int side) const;       // why it is not available
    D2D1_RECT_F TestPill(const PopoverLayout& g, int side) const;
    void ToggleTest(int side);
    void StopRunningTest();
    int MenuRows(int side) const {
        if (side == 3) return settingsPage_ == 0 ? 7 : settingsPage_ == 1 ? 5 : settingsPage_ == 3 ? 2 : 1 + static_cast<int>(MuteRows().size());
        return std::max<int>(1, static_cast<int>(side == 2 ? camera_.devices.size() : VoiceSide(side).devices.size()));
    }
    int MaxRows(int side) const;                 // rows visible at once
    bool SoluActive() const;                     // a conversation is on: listening, thinking or speaking
    D2D1_RECT_F SoluCaptionRect(const D2D1_RECT_F& base) const;
    void DrawSoluCaption(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    void DrawSolu(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);               // the Solu AI tab
    void DrawSettingsMenu(ID2D1DeviceContext* dc, const PopoverLayout& g, float a);      // the gear's menu
    void DrawSoluBar(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar, float alpha);       // the living indicator on the thin bar
    bool SoluAwake() const { return solu_.state == SoluState::Ready || SoluActive(); }
    bool CaptionsShown() const { return solu_.captions && SoluActive() && !captionsYielded_ && permissions_.empty(); }   // the live view; an approval card comes first
    void ToggleMute(int side);                   // mute the mic / speaker, or block the camera
    D2D1_RECT_F MuteSwitch(const PopoverLayout& g) const;
    PopoverLayout PopoverGeometry(const D2D1_RECT_F& island) const;   // the menu area inside the island
    float MenuHeight(int side) const;                                 // island height that fits that menu
    const AudioSide& VoiceSide(int side) const { return side == 0 ? audio_.input : audio_.output; }
    void DrawAudioButtons(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    void DrawAudioPopover(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    void UpdateAudioWatch();
    void OpenPopover(int side);                  // -1 closes
    bool InsideZone(D2D1_POINT_2F p, float slack) const;   // the island, with a little slack
    float TargetHeight() const;                  // panel height the current tab wants
    void ApplyHeightTarget();                    // spring to it (and resize the clickable area)
    void DrawPermission(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    std::array<D2D1_RECT_F, 3> PermissionButtons(const D2D1_RECT_F& island) const;
    float PermissionDetailH() const;             // the detail box grows to show a whole email / long command (Solu)  // reject, approve, defer
    void UpdateArtwork();
    void DrawMascot(float opacity);
    void DrawMascotEffects(ID2D1DeviceContext* dc, float opacity);
    void DrawText(ID2D1DeviceContext* dc, const wchar_t* text, IDWriteTextFormat* fmt, D2D1_RECT_F r,
                  D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT vertical = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

#ifndef NDEBUG
    void WriteSnapshot();
    std::wstring snapshotPath_;                  // DebugSnapshot: the frame being rendered goes here
#endif

    Gfx& gfx_;
    HMONITOR monitor_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    float scale_ = 1.0f;
    UINT pxW_ = 0, pxH_ = 0;
    bool suppressed_ = false;
    bool deviceLost_ = false;

    Com<IDXGISwapChain1> swap_;
    Com<IDCompositionTarget> target_;
    Com<IDCompositionVisual> visual_;
    Com<ID2D1DeviceContext> dc_;
    Com<ID2D1Bitmap1> backbuffer_;
    Com<ID3D11RenderTargetView> rtv_;
    Com<ID2D1SolidColorBrush> brush_;
    Com<ID2D1Effect> shadow_;

    std::shared_ptr<const MascotAsset> mascotAsset_;
    MascotRenderer* mascotRenderer_ = nullptr;
    std::unique_ptr<Mascot> mascot_;
    std::vector<DirectX::XMFLOAT4X4> skin_;

    bool expanded_ = false;
    bool frameQueued_ = false;
    bool panelMoving_ = false;
    bool tracking_ = false;
    bool fullRegion_ = false;
    bool autoClosePending_ = false;              // opened itself for a notification; closes on a timer
    int hover_ = -1;
    int pressed_ = -1;
    LARGE_INTEGER lastFrame_{};

    Spring w_, h_;
    Spring tabX_, tabW_;                         // selection pill
    std::array<float, kSectionCount> tabHover_{};            // 0..1 hover highlight per tab
    std::array<float, kSectionCount> sectionAlpha_{1};                   // cross-fade between sections (Music shows first)
    Section section_ = Section::Music;

    MediaInfo media_;
    Lyrics lyrics_;
    bool lyricsOn_ = true;
    int lyricLine_ = -1, lyricPrev_ = -1;        // the line being sung, and the one fading out
    float lyricT_ = 1.0f;                        // 0 -> 1: the change from lyricPrev_ to lyricLine_
    float lyricsHover_ = 0.0f;                   // the lyrics button in the Music tab
    std::function<void(bool)> lyricsHandler_;
    std::function<void(MediaCommand, double)> mediaHandler_;
    bool seeking_ = false;                       // the progress bar is being dragged
    float seekFraction_ = 0, seekHover_ = 0;
    struct SeekBar { float x0, x1, y; };
    SeekBar SeekGeometry(const D2D1_RECT_F& panel) const;
    float SeekFractionAt(float x, const D2D1_RECT_F& panel) const;
    Com<ID2D1Bitmap1> artwork_;
    uint64_t artworkId_ = 0;
    std::array<float, 3> controlHover_{};        // previous, play/pause, next
    std::array<float, 3> extraHover_{};          // shuffle, repeat, the output chip

    NotificationSnapshot notifications_;
    std::function<void(uint32_t)> dismissHandler_;
    std::function<void()> clearAllHandler_;
    std::unordered_map<uint32_t, Com<ID2D1Bitmap1>> notificationIcons_;
    float notifScroll_ = 0, notifScrollTarget_ = 0;   // first visible row (fractional while it glides)
    std::unordered_map<uint32_t, float> notifCloseHover_;   // per notification id
    float notifClearHover_ = 0;
    void OnWheel(int delta);

    AudioSnapshot audio_;
    CameraSnapshot camera_;
    SoluSnapshot solu_;
    std::function<void(bool)> soluEnable_, soluHeadphones_, soluCaptions_, soluScreen_;
    Com<ID2D1Bitmap1> lookBitmap_;               // the picture of what Solu is looking at (rebuilt when a new one arrives)
    const ImageBGRA* lookKey_ = nullptr;
    float captionEndHover_ = 0;                  // the "End" pill of the live view
    bool clickAwayOn_ = false;
    void UpdateClickAway();                      // watch for clicks outside the island only while the live view is up
    bool captionsYielded_ = false;
    // ---- Shelf tab
    ShelfSnapshot shelf_;
    std::function<void(const std::vector<std::wstring>&)> shelfAdd_;
    std::function<void(const std::wstring&)> shelfRemove_;
    std::function<void()> shelfClear_;
    std::set<std::wstring> shelfSel_;                            // selected items (paths)
    std::unordered_map<std::wstring, Com<ID2D1Bitmap1>> shelfBitmaps_;
    std::unordered_map<std::wstring, float> shelfPop_;           // 0 -> 1 as an item appears
    std::set<std::wstring> shelfSeen_;
    std::vector<float> shelfHover_;
    float shelfScroll_ = 0, dragT_ = 0;
    bool dragOver_ = false, dragOpened_ = false, shelfDragging_ = false;
    enum class DropZone { Shelf, SafeShare };
    DropZone dropZone_ = DropZone::Shelf;                    // the zone under the files being dragged
    float dropShare_ = 0;                                    // 0 -> 1 as the SafeShare zone becomes the one under them
    DropZone DropZoneAt(POINT screen) const;
    bool ShareTakesDrops() const { return !ShareBusy(); }   // (a send on its way: its files can't change)
    void DrawDropChooser(ID2D1DeviceContext* dc, const D2D1_RECT_F& island, float alpha);
    int shelfPressed_ = -1, shelfLastItem_ = -1;
    POINT shelfPressPt_{};
    ULONGLONG shelfLastClick_ = 0;
    IDropTarget* dropTarget_ = nullptr;
    D2D1_RECT_F ShelfCard(const D2D1_RECT_F& island, int index) const;
    D2D1_RECT_F ShelfArea(const D2D1_RECT_F& island) const;
    void DrawShelf(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    ID2D1Bitmap1* ShelfBitmap(const ShelfItem& item);
    void StartShelfDrag(int index);
    void ShelfAddFiles();
    void OpenShelfItem(int index, bool reveal);
    // ---- Agents tab
    AgentSnapshot agents_;
    AgentTool agentTool_ = AgentTool::Claude;
    std::wstring agentSession_;                  // chosen session id ("" = the best one of that agent)
    bool agentAuto_ = true;                      // follow whichever agent is working until the user picks one
    int agentScroll_ = 0;                        // feed rows scrolled up from the newest
    TextField prompt_;
    std::wstring agentError_;
    bool newChat_ = false;
    HWND prevForeground_ = nullptr;
    uint32_t agentVersionSeen_ = 0;
    std::function<bool(AgentTool, const std::wstring&, const std::wstring&, const std::wstring&, bool, std::wstring&)> agentSend_;
    std::function<void(AgentTool)> agentConnect_;
    std::function<void(const AgentSession&, bool)> agentOpen_;
    std::function<void(const std::string&, const std::string&)> taskHandler_;
    int SoluTaskRows() const;                    // how many task rows the Solu AI tab shows
    Com<ID2D1Bitmap1> logoClaude_, logoCodex_;
    struct AgentsLayout {
        D2D1_RECT_F tool[2], connect, prev, next, header, feed, newBtn, box, send;
        int rows = 0;
    };
    AgentsLayout AgentsGeometry(const D2D1_RECT_F& island) const;
    const AgentSession* CurrentSession() const;
    std::vector<const AgentSession*> SessionsOf(AgentTool tool) const;
    void AutoPickAgent();
    void DrawAgents(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    ID2D1Bitmap1* Logo(AgentTool tool);
    void SendPrompt();
    void UpdateAgentTimer();
    // ---- typing (the Agents prompt, the reminder boxes): one box has the keyboard at a time
    TextField* field_ = nullptr;                 // the box being typed in (null: none; the island never takes focus then)
    void Focus(TextField& field);                // the island takes the keyboard (it lets go of it again in Blur)
    void Blur(bool restoreWindow);               // restoreWindow: back to the window the user was in
    void OnKey(WPARAM vk);
    void OnChar(wchar_t ch);
    // `right`: room kept free at the right end (a chip or buttons inside the box); `secret`: dots instead of the text.
    void DrawField(ID2D1DeviceContext* dc, TextField& field, const D2D1_RECT_F& box, const wchar_t* hint, float a, float right = 0,
                   bool secret = false);
    int FieldSection(const TextField* field) const;   // the tab a text box is on (-1: none)
    bool ViewKey(WPARAM vk);                     // Enter / Tab / arrows in a tab's boxes (true when handled)
    // ---- Reminders tab (RemindersView.cpp)
    ReminderSnapshot reminders_;
    TextField reminderWhat_, reminderNote_;
    bool reminderFlip_ = false;                  // the user flipped the guessed AM / PM of what is typed
    int reminderScroll_ = 0;                     // first visible row
    std::function<void(const std::wstring&, const std::wstring&, int64_t, Repeat, int)> reminderAdd_;
    std::function<void(uint32_t)> reminderRemove_, reminderDismiss_;
    std::function<void(uint32_t, int)> reminderSnooze_;
    struct RemindersLayout {
        D2D1_RECT_F list, what, note, add, preview;
        int rows = 0;                            // rows that fit in the list
    };
    RemindersLayout RemindersGeometry(const D2D1_RECT_F& island) const;
    When ReminderPreview() const;                // what is typed, understood
    void DrawReminders(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    void DrawAlarm(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);   // the card while a reminder rings
    std::array<D2D1_RECT_F, 2> AlarmButtons(const D2D1_RECT_F& island) const;   // snooze, dismiss
    int RemindersHit(D2D1_POINT_2F p, const D2D1_RECT_F& island) const;
    void RemindersClick(int id);
    bool RemindersKey(WPARAM vk);                // Enter / Tab / Esc in a reminder box (true when handled)
    void AddReminder();
    // ---- SafeShare tab (SafeShareView.cpp)
    struct ShareFile {
        std::wstring path, name;
        uint64_t size = 0;
    };
    struct ShareLayout {
        D2D1_RECT_F modeEmail, modePhone, sentList, back;        // the header row
        D2D1_RECT_F files, addMore, clear;                       // the drop area, and its "Add more" / "Clear"
        D2D1_RECT_F name, email, password, reveal, generate, send;
        std::array<D2D1_RECT_F, 3> devices;                      // 1, 3, 5 devices
        D2D1_RECT_F panel;                                       // under the files: the progress, the result, the phone link
        D2D1_RECT_F primary, secondary, tertiary;                // the panel's buttons, right to left
        D2D1_RECT_F status;                                      // the line at the bottom (Email mode)
        D2D1_RECT_F list;                                        // the Sent view
        int rows = 0;                                            // sent rows that fit
    };
    struct ShareChip {
        D2D1_RECT_F box, close;
        int file = -1;                                           // index into shareFiles_ (-1: the "+N more" chip)
    };
    SafeShareSnapshot shareSnap_;
    SafeShareHandlers share_;
    std::vector<ShareFile> shareFiles_;
    TextField shareName_, shareEmail_, sharePassword_;
    bool sharePhone_ = false;                    // the Phone mode (else Email)
    bool shareSentView_ = false;                 // the list of sent rooms
    bool shareReveal_ = false;                   // the password is shown
    int shareDevices_ = 1;
    int shareScroll_ = 0;                        // first visible sent row
    std::wstring shareNote_;                     // a short message from the last action ("Folders can't be sent")
    ULONGLONG shareNoteUntil_ = 0;
    bool shareNoteGood_ = false;
    std::wstring shareConfirm_;                  // the room whose delete mark was pressed once (a second press deletes)
    ULONGLONG shareConfirmUntil_ = 0;
    bool shareSeeded_ = false;                   // the name and device choice were filled in from the last send
    ShareLayout ShareGeometry(const D2D1_RECT_F& island) const;
    std::vector<ShareChip> ShareChips(const ShareLayout& g) const;   // the file chips that fit in the drop area
    void DrawSafeShare(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    void DrawShareFiles(ID2D1DeviceContext* dc, const ShareLayout& g, float a);
    void DrawShareForm(ID2D1DeviceContext* dc, const ShareLayout& g, float a);
    void DrawShareProgress(ID2D1DeviceContext* dc, const ShareLayout& g, float a);
    void DrawShareResult(ID2D1DeviceContext* dc, const ShareLayout& g, float a);
    void DrawSharePhone(ID2D1DeviceContext* dc, const ShareLayout& g, float a);
    void DrawShareSent(ID2D1DeviceContext* dc, const ShareLayout& g, float a);
    void DrawShareStatus(ID2D1DeviceContext* dc, const ShareLayout& g, float a);
    void DrawShareButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, const wchar_t* label, bool primary, bool enabled, bool hot, float a);
    int SafeShareHit(D2D1_POINT_2F p, const D2D1_RECT_F& island) const;
    void SafeShareClick(int id);
    bool SafeShareKey(WPARAM vk);                // Enter / Tab in a SafeShare box (true when handled)
    void AddShareFiles(const std::vector<std::wstring>& paths);
    void ChooseShareFiles();
    void SendShare();
    std::wstring ShareProblem() const;           // why Send cannot be pressed yet ("" = it can)
    bool ShareBusy() const;                      // a send is in progress
    void ShareSay(const std::wstring& note, bool good = false);   // a short line under the form (good: green, else amber)
    void CopyShareLink(const std::wstring& roomId);
    void ResetShareForm();                       // after a send: the files, recipient and password go; name and choice stay
    void OfferShareFiles();                      // the phone sees the current files
    std::vector<Activity> activities_;
    std::function<void(const std::wstring&)> activityDismiss_;
    std::function<void(const Activity&)> mark_;
    std::unordered_map<const ImageBGRA*, Com<ID2D1Bitmap1>> thumbs_;   // pill thumbnails (screenshots)
    ID2D1Bitmap1* Thumb(const ImageBGRA& image);
    // ---- the closed island: it shows one thing (a "glimpse"); anything else going on waits as a small icon at its right
    // end. Activities come first (an agent at work, a download, the focus timer), then the music. A middle click on an icon
    // brings that one to the front; the one shown takes its place among the icons.
    struct Glimpse {
        std::wstring id;                         // an activity's id, or L"music"
        std::wstring glyph;
        D2D1_COLOR_F tint;
    };
    std::vector<Glimpse> Glimpses() const;
    std::wstring FrontGlimpse() const;           // the one shown: the user's pick while it lasts, else the first
    const Activity* FrontActivity() const;       // ... when that is an activity
    std::vector<D2D1_RECT_F> GlimpseIcons(const D2D1_RECT_F& pill) const;   // the others' icons, right to left
    float GlimpseIconsW() const;
    void DrawGlimpseIcons(ID2D1DeviceContext* dc, const D2D1_RECT_F& pill, float a);
    void OnMiddleClick(POINT screen);            // on an icon: to the front; on the close mark: dismiss; elsewhere: open
    bool MusicGlimpse() const;                   // music is playing (with lyrics or not)
    std::wstring frontGlimpse_;
    bool Live() const { return FrontActivity() != nullptr; }   // the closed island shows an activity
    bool LyricsReady() const;                    // synced lyrics for the playing song, and lyrics are on
    bool MusicPill() const;                      // the closed island shows the music (the line being sung, or the song)
    float LyricShift() const;                    // how much lower the Music tab's bar and buttons sit for the lyric lines
    void UpdateLyricLine();                      // which line is being sung now; schedules the next change
    void RefreshCollapsed();                     // the closed island's size follows what it shows
    D2D1_POINT_2F LyricsButton(const D2D1_RECT_F& island) const;
    void DrawMusicPill(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float alpha);
    void DrawLyricLines(ID2D1DeviceContext* dc, float x0, float x1, float top, float alpha);
    float CollapsedW() const;                    // the idle bar, or the wider live pill
    float CollapsedH() const;
    D2D1_RECT_F LiveClose(const D2D1_RECT_F& pill) const;
    void DrawLive(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float alpha);               // the user picked a tab / menu during a conversation: the live view steps aside until it ends
    std::function<void()> soluTalk_, soluStop_;
    std::function<void(const std::wstring&)> soluVoice_;
    int settingsPage_ = 0;                       // the gear's menu: 0 the options, 1 Solu's voices, 2 notifications, 3 the closed island
    bool glimpseLyrics_ = true, glimpseAgents_ = true;   // what the closed island may show (Settings > Closed island)
    std::function<void(bool, bool)> glimpseHandler_;
    NotificationMutes mutes_;
    std::function<void(const std::wstring&, const std::wstring&, bool)> muteApp_;
    std::function<void(bool)> muteAll_;
    std::vector<MutedApp> MuteRows() const;      // the apps on the notifications page: the ones seen now, then muted ones
    float barLevel_ = 0, micImpulse_ = 0;        // the thin bar's voice meter and the last burst of the microphone
    float captionT_ = 0;                         // the live conversation view fades in over the tab content
    bool soluWake_ = false;                      // the island opened for Solu: stay open until it is awake
    std::array<float, 24> soluBars_{};           // recent microphone levels while Solu listens
    std::function<void(int, bool)> muteHandler_;
    std::function<void(bool, bool)> audioTestHandler_;
    std::array<float, 32> testBars_{};           // recent levels of a running test, newest last
    float testHover_ = 0;
    ULONGLONG lastTestDone_ = 0;
    std::function<void(bool)> audioWatchHandler_;
    std::function<void(bool, const std::wstring&)> audioSelectHandler_;
    bool audioWatching_ = false;
    int popover_ = -1;                           // which button's menu is open (-1 none)
    int popoverSide_ = -1;                       // the side still being drawn while the menu fades out
    float popoverT_ = 0;                         // 0 closed .. 1 open
    int voiceScroll_ = 0;                        // first visible row of the open menu
    std::array<float, 4> audioHover_{}, audioOpen_{}, voiceLevel_{}, muteAnim_{1, 1, 1, 1};
    std::array<float, 7> popRowHover_{};          // (the settings menu shows up to 7 rows)
    float popFooterHover_ = 0;

    SystemSnapshot system_;
    std::function<void(bool)> systemWatchHandler_;
    bool watching_ = false;
    bool debugPinned_ = false;
    Com<ID2D1StrokeStyle> roundStroke_;
    std::array<int, kMetricCount> ringPage_{};       // page the user chose
    std::array<int, kMetricCount> ringShown_{};      // page currently drawn (lags during the cross-fade)
    std::array<float, kMetricCount> ringFade_{1, 1, 1, 1}, ringHover_{}, ringPress_{}, ringFrac_{};

    // ---- the welcome tour (TourView.cpp)
    struct TourLayout {
        D2D1_RECT_F header, skip, stage, text, primary, secondary, back, toggle;
        std::array<D2D1_RECT_F, 7> dots{};      // one per step after the invite
    };
    struct TourAction {                          // the step's buttons and its one line of status
        std::wstring primary, secondary, status;
        int statusKind = 0;                      // 0 none, 1 done (green), 2 waiting (spinner), 3 needs attention (amber)
        bool primaryAdvances = true;             // the primary button only moves on (else it does the step's thing)
        bool secondaryOpensSettings = false;
    };
    bool TourInvite() const { return tourOn_ && !expanded_; }
    bool TourShown() const;                      // the panel shows the tour (an approval card or a ringing reminder comes first)
    TourLayout TourGeometry(const D2D1_RECT_F& island) const;
    TourAction TourActions() const;
    int TourHit(D2D1_POINT_2F p, const D2D1_RECT_F& island) const;
    void TourClick(int id);
    void TourGo(int step);                       // to another step, with the slide
    void TourEnter(TourStep step);               // what the cat does when a step begins
    void TourLeave(TourStep step);
    bool TourAnimate(float dt);                  // per-frame state; true while something moves
    void DrawTour(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    void DrawTourInvite(ID2D1DeviceContext* dc, const D2D1_RECT_F& pill, float alpha);
    void DrawTourStep(ID2D1DeviceContext* dc, TourStep step, const TourLayout& g, float a, float dx, float time);
    void DrawTourScene(ID2D1DeviceContext* dc, TourStep step, D2D1_RECT_F stage, float a, float time);
    void TourFinish(bool completed);             // completed: the cat cheers, then the island closes; else it closes now
    float TourInviteW() const;
    void DrawLogoMark(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float size, float a, bool tile);
    void DrawCursor(ID2D1DeviceContext* dc, D2D1_POINT_2F tip, float a, float press);
    void TourRipple(ID2D1DeviceContext* dc, D2D1_POINT_2F at, float phase, float a);
    void TourMiniIsland(ID2D1DeviceContext* dc, D2D1_RECT_F r, float a);
    void TourDesktop(ID2D1DeviceContext* dc, D2D1_RECT_F stage, float a, bool windows);
    void TourCaption(ID2D1DeviceContext* dc, D2D1_RECT_F stage, const wchar_t* text, float a);
    void TourKeycap(ID2D1DeviceContext* dc, D2D1_RECT_F r, const wchar_t* label, float press, float a);
    void TourQr(ID2D1DeviceContext* dc, D2D1_RECT_F r, float a, bool light);
    void SceneHello(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time);
    void SceneGlance(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time);
    void SceneNotifications(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time);
    void SceneAgents(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time);
    void SceneFiles(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time);
    void SceneSolu(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time);
    void SceneDone(ID2D1DeviceContext* dc, D2D1_RECT_F s, float a, float time);
    ID2D1StrokeStyle* TourStroke();              // round caps and joins
    ID2D1PathGeometry* TourPath(int which);      // cached shapes: 0 pointer, 1 the mark's ears, 2 its eyes, 3 a document
    bool tourOn_ = false;
    int tourStep_ = 0, tourPrev_ = -1;           // the step shown, and the one sliding away
    int tourSeen_ = 0;                           // the furthest step reached (its dots can be clicked)
    float tourTime_ = 0;                         // seconds since the step began (the scene's clock)
    float tourPrevTime_ = 0;
    float tourSwap_ = 1;                         // 0 -> 1: the slide from tourPrev_ to tourStep_
    int tourDir_ = 1;
    float tourClock_ = 0;                        // the invite's glow
    bool tourAskedNotifications_ = false, tourAskedPhone_ = false, tourAskedAgents_ = false, tourAskedSolu_ = false;
    bool tourDoneCheered_[static_cast<int>(TourStep::Count)]{};
    bool tourFinishing_ = false;
    std::array<float, 5> tourHover_{};           // primary, secondary, back, skip, switch
    TourStatus tourStatus_;
    TourHandlers tour_;
    bool soluPlanReady_ = false;
    SoluSetupPlan soluPlan_;
    SoluSetupProgress soluSetupProgress_;
    bool reducedMotion_ = false;                 // Windows' "Animation effects" is off: still scenes
    std::array<Com<ID2D1PathGeometry>, 4> tourPaths_;
    Com<ID2D1StrokeStyle> tourStroke_;

    std::vector<PermissionRequest> permissions_;  // oldest first; the first one is on screen
    std::function<void(uint64_t, PermissionDecision)> permissionHandler_;
    std::array<float, 3> permissionHover_{};      // reject, approve, defer
    uint64_t cardId_ = 0, cardShownAt_ = 0;        // the card on screen, and when it appeared (no clicks in its first moment)
};

} // namespace eflow
