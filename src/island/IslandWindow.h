#pragma once
#include <windows.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "core/Gfx.h"
#include "island/Spring.h"
#include "mascot/Mascot.h"
#include "services/MediaService.h"
#include "services/PermissionServer.h"

namespace eflow {

class MascotRenderer;

// One island per monitor: a borderless, always-on-top, non-activating popup whose pixels come from a
// DirectComposition swap chain (true per-pixel alpha). The window never moves or resizes while
// animating; a window region keeps everything outside the idle bar click-through. The mascot lives
// inside the open panel and only renders while the panel is visible.
class IslandWindow {
public:
    enum class Section { Music, Notifications, Ai };

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
    void SetMediaHandler(std::function<void(MediaCommand)> handler) { mediaHandler_ = std::move(handler); }
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
    std::array<D2D1_RECT_F, 3> TabRects(const D2D1_RECT_F& island) const;
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
    void DrawPermission(ID2D1DeviceContext* dc, D2D1_RECT_F base, float alpha);
    std::array<D2D1_RECT_F, 3> PermissionButtons(const D2D1_RECT_F& island) const;  // reject, approve, defer
    void UpdateArtwork();
    void DrawMascot(float opacity);
    void DrawMascotEffects(ID2D1DeviceContext* dc, float opacity);
    void DrawText(ID2D1DeviceContext* dc, const wchar_t* text, IDWriteTextFormat* fmt, D2D1_RECT_F r,
                  D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

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
    std::array<float, 3> tabHover_{};            // 0..1 hover highlight per tab
    std::array<float, 3> sectionAlpha_{1, 0, 0}; // cross-fade between sections
    Section section_ = Section::Music;

    MediaInfo media_;
    std::function<void(MediaCommand)> mediaHandler_;
    Com<ID2D1Bitmap1> artwork_;
    uint64_t artworkId_ = 0;
    std::array<float, 3> controlHover_{};        // previous, play/pause, next

    std::vector<PermissionRequest> permissions_;  // oldest first; the first one is on screen
    std::function<void(uint64_t, PermissionDecision)> permissionHandler_;
    std::array<float, 3> permissionHover_{};      // reject, approve, defer
};

} // namespace eflow
