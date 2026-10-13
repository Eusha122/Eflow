#pragma once
#include <DirectXMath.h>

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "mascot/MascotAsset.h"

namespace eflow {

// Behaviour + animation for one on-screen mascot: picks the clip and facial expression for the
// current state, cross-fades between clips, schedules blinks, and produces skinning matrices.
class Mascot {
public:
    enum class State { Idle, Music, Notify, Permission };

    explicit Mascot(std::shared_ptr<const MascotAsset> asset);

    // Layers, highest priority first: a notification reaction (one-shot), a pending permission
    // request (held until resolved), then the ambient mood (Idle or Music). Each layer is set
    // independently, so e.g. music stopping never cancels a pending request.
    void SetAmbient(State ambient);              // Idle or Music
    void Notify();                               // one-shot reaction; also bumps the badge
    void SetPermissionPending(bool pending);
    State state() const { return state_; }       // what is showing right now
    State baseState() const { return base_; }    // the ambient mood underneath
    bool permissionPending() const { return permission_; }

    void Update(float dt);
    // Joint matrices for the vertex shader (row-vector convention): inverseBind * jointWorld.
    void ComputeSkin(std::vector<DirectX::XMFLOAT4X4>& out) const;
    // Expression whose face patches to draw ("neutral" = none).
    const char* Face() const;

    float stateTime() const { return stateTime_; }   // seconds since the current state began
    float time() const { return time_; }             // monotonically increasing, for effects
    bool calm() const { return state_ == State::Idle && blend_ >= 1.0f; }

    int badge() const { return badge_; }
    void ClearBadge() { badge_ = 0; }

private:
    struct Playback {
        int clip = -1;
        float t = 0;
    };
    void Enter(State s);                 // unconditional transition with cross-fade
    State Resting() const { return permission_ ? State::Permission : base_; }   // where to settle after a reaction
    void Sample(const Playback& p, std::vector<MascotAsset::TRS>& pose) const;
    int ClipFor(State s) const;

    std::shared_ptr<const MascotAsset> asset_;
    State state_ = State::Idle;
    State base_ = State::Idle;
    bool permission_ = false;
    Playback current_, previous_;
    float blend_ = 1.0f;                 // 0 -> previous clip, 1 -> current clip
    float stateTime_ = 0, time_ = 0;
    float nextBlink_ = 3.0f, blinkLeft_ = 0;
    bool secondBlink_ = false;
    int badge_ = 0;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace eflow
