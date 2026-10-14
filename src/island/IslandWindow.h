#pragma once
#include <windows.h>

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/Gfx.h"
#include "island/Spring.h"
#include "mascot/Mascot.h"
#include "services/AudioDevices.h"
#include "services/CameraService.h"
#include "services/MediaService.h"
#include "services/NotificationService.h"
#include "services/PermissionServer.h"
#include "services/SoluService.h"
#include "services/SystemMonitor.h"

namespace eflow {

class MascotRenderer;

inline constexpr int kSectionCount = 5;   // Music, Notifications, Messages, AI Requests, System
inline constexpr int kMetricCount = 4;    // CPU, GPU, Memory, Network

// One island per monitor: a borderless, always-on-top, non-activating popup whose pixels come from a
// DirectComposition swap chain (true per-pixel alpha). The window never moves or resizes while
// animating; a window region keeps everything outside the idle bar click-through. The mascot lives
// inside the open panel and only renders while the panel is visible.
class IslandWindow {
public:
    enum class Section { Music, Notifications, Solu, Ai, System };

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
    void OnNotification();                       // opens briefly on the Notifications tab
    void SetPermissionPending(bool pending);     // opens on the AI tab and stays open until resolved
    void SetMedia(const MediaInfo& media);
    void SetMediaHandler(std::function<void(MediaCommand, double)> handler) { mediaHandler_ = std::move(handler); }
    void SetNotifications(const NotificationSnapshot& snapshot);
    void SetNotificationHandlers(std::function<void(uint32_t)> dismiss, std::function<void()> clearAll) {
        dismissHandler_ = std::move(dismiss);
        clearAllHandler_ = std::move(clearAll);
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
    void ShowForSolu();                          // the hotkey was pressed: open so the conversation is visible
    void SetSoluHandlers(std::function<void(bool)> enable, std::function<void()> talk, std::function<void(const std::wstring&)> voice,
                         std::function<void(bool)> headphones, std::function<void()> stop, std::function<void(bool)> captions) {
        soluEnable_ = std::move(enable);
        soluTalk_ = std::move(talk);
        soluVoice_ = std::move(voice);
        soluHeadphones_ = std::move(headphones);
        soluStop_ = std::move(stop);
        soluCaptions_ = std::move(captions);
    }
#ifndef NDEBUG
    void DebugOpenTab(int section);              // scripted tests: open on a tab and keep it open (-1 releases)
#endif
    // Called with true while the System tab is on screen (so the monitor samples), false otherwise.
    void SetSystemWatchHandler(std::function<void(bool)> handler) { systemWatchHandler_ = std::move(handler); }
    void SetPermissions(const std::vector<PermissionRequest>& pending);
    void SetPermissionHandler(std::function<void(uint64_t, PermissionDecision)> handler) {
        permissionHandler_ = std::move(handler);
    }

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

    // state machine
    void Expand();
    void Collapse();
    void OnMouseMove(D2D1_POINT_2F p);
    int HitTest(D2D1_POINT_2F p) const;          // -1 = none, 0..2 = tabs, kHitPrev.. = media controls
    void OnClick(int id);

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
        if (side == 3) return settingsPage_ == 1 ? 5 : 4;    // Settings: the options, or Solu's voices
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
    bool CaptionsShown() const { return solu_.captions && SoluActive(); }                 // the live conversation view
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
    std::array<D2D1_RECT_F, 3> PermissionButtons(const D2D1_RECT_F& island) const;  // reject, approve, defer
    void UpdateArtwork();
    void DrawMascot(float opacity);
    void DrawMascotEffects(ID2D1DeviceContext* dc, float opacity);
    void DrawText(ID2D1DeviceContext* dc, const wchar_t* text, IDWriteTextFormat* fmt, D2D1_RECT_F r,
                  D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT vertical = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

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
    bool openPending_ = false;
    bool fullRegion_ = false;
    bool autoClosePending_ = false;              // opened itself for a notification; closes on a timer
    int hover_ = -1;
    int pressed_ = -1;
    LARGE_INTEGER lastFrame_{};

    Spring w_, h_;
    Spring tabX_, tabW_;                         // selection pill
    std::array<float, kSectionCount> tabHover_{};            // 0..1 hover highlight per tab
    std::array<float, kSectionCount> sectionAlpha_{1, 0, 0, 0, 0}; // cross-fade between sections
    Section section_ = Section::Music;

    MediaInfo media_;
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
    std::function<void(bool)> soluEnable_, soluHeadphones_, soluCaptions_;
    std::function<void()> soluTalk_, soluStop_;
    std::function<void(const std::wstring&)> soluVoice_;
    int settingsPage_ = 0;                       // the gear's menu: 0 the options, 1 Solu's voices
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
    std::array<float, 6> popRowHover_{};
    float popFooterHover_ = 0;

    SystemSnapshot system_;
    std::function<void(bool)> systemWatchHandler_;
    bool watching_ = false;
    bool debugPinned_ = false;
    Com<ID2D1StrokeStyle> roundStroke_;
    std::array<int, kMetricCount> ringPage_{};       // page the user chose
    std::array<int, kMetricCount> ringShown_{};      // page currently drawn (lags during the cross-fade)
    std::array<float, kMetricCount> ringFade_{1, 1, 1, 1}, ringHover_{}, ringPress_{}, ringFrac_{};

    std::vector<PermissionRequest> permissions_;  // oldest first; the first one is on screen
    std::function<void(uint64_t, PermissionDecision)> permissionHandler_;
    std::array<float, 3> permissionHover_{};      // reject, approve, defer
};

} // namespace eflow
