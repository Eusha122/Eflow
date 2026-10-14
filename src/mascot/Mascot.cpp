#include "mascot/Mascot.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace eflow {
namespace {

constexpr float kCrossFade = 0.28f;      // seconds to blend from one clip into the next
constexpr float kBlinkLength = 0.13f;

// Value of one channel at time t (LINEAR or STEP), written into `out` (3 or 4 floats).
void SampleChannel(const MascotAsset::Channel& ch, float t, float* out, int width) {
    const auto& times = ch.times;
    if (times.empty()) return;
    if (t <= times.front() || times.size() == 1) {
        std::copy_n(ch.values.data(), width, out);
        return;
    }
    if (t >= times.back()) {
        std::copy_n(ch.values.data() + (times.size() - 1) * width, width, out);
        return;
    }
    const size_t hi = std::upper_bound(times.begin(), times.end(), t) - times.begin();
    const size_t lo = hi - 1;
    const float* a = ch.values.data() + lo * width;
    const float* b = ch.values.data() + hi * width;
    if (ch.step) {
        std::copy_n(a, width, out);
        return;
    }
    const float f = (t - times[lo]) / (times[hi] - times[lo]);
    if (width == 4) {                    // quaternion: shortest-path normalised lerp
        const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const float s = dot < 0 ? -1.0f : 1.0f;
        float len = 0;
        for (int i = 0; i < 4; ++i) {
            out[i] = a[i] + (s * b[i] - a[i]) * f;
            len += out[i] * out[i];
        }
        len = std::sqrt(len);
        for (int i = 0; i < 4; ++i) out[i] /= len;
    } else {
        for (int i = 0; i < width; ++i) out[i] = a[i] + (b[i] - a[i]) * f;
    }
}

XMMATRIX ToMatrix(const MascotAsset::TRS& p) {
    return XMMatrixScaling(p.s.x, p.s.y, p.s.z) * XMMatrixRotationQuaternion(XMLoadFloat4(&p.r)) *
           XMMatrixTranslation(p.t.x, p.t.y, p.t.z);
}

} // namespace

Mascot::Mascot(std::shared_ptr<const MascotAsset> asset) : asset_(std::move(asset)) {
    current_.clip = ClipFor(State::Idle);
}

int Mascot::ClipFor(State s) const {
    switch (s) {
    case State::Music: return asset_->FindClip("music");
    case State::Notify: return asset_->FindClip("notify");
    case State::Permission: return asset_->FindClip("permission");
    default: return asset_->FindClip("idle");
    }
}

void Mascot::SetAmbient(State ambient) {
    base_ = ambient == State::Music ? State::Music : State::Idle;
    if (state_ != State::Notify && state_ != Resting()) Enter(Resting());   // a running reaction settles itself
}

void Mascot::Notify() {
    ++badge_;
    Enter(State::Notify);                      // restarts the reaction even if already playing
}

void Mascot::SetPermissionPending(bool pending) {
    permission_ = pending;
    if (state_ != State::Notify && state_ != Resting()) Enter(Resting());
}

void Mascot::SetTalk(Talk talk) {
    if (talk == talk_) return;
    talk_ = talk;
    mouthOpen_ = false;
    if (state_ != State::Notify && state_ != Resting()) Enter(Resting());
}

void Mascot::Enter(State s) {
    state_ = s;
    stateTime_ = 0;
    previous_ = current_;
    current_ = {ClipFor(s), 0.0f};
    blend_ = previous_.clip >= 0 ? 0.0f : 1.0f;
}

void Mascot::Update(float dt) {
    time_ += dt;
    stateTime_ += dt;
    blend_ = std::min(1.0f, blend_ + dt / kCrossFade);

    auto advance = [&](Playback& p, bool loop) {
        if (p.clip < 0) return;
        p.t += dt;
        const float d = asset_->clips[p.clip].duration;
        if (loop && d > 0) p.t = std::fmod(p.t, d);
    };
    advance(current_, state_ != State::Notify);
    advance(previous_, true);

    if (state_ == State::Notify && current_.clip >= 0 && current_.t >= asset_->clips[current_.clip].duration)
        Enter(Resting());

    if (talk_ == Talk::Speaking) {                // mouth: open while loud, held a moment so it does not flicker
        if (talkLevel_ > 0.13f) mouthHold_ = 0.09f;
        mouthOpen_ = mouthHold_ > 0;
        mouthHold_ -= dt;
    }

    // Blinks (shown only with the neutral face): every few seconds, sometimes a quick double blink.
    if (blinkLeft_ > 0) {
        blinkLeft_ -= dt;
    } else if ((nextBlink_ -= dt) <= 0) {
        blinkLeft_ = kBlinkLength;
        std::uniform_real_distribution<float> gap(2.6f, 5.8f);
        if (secondBlink_) {
            secondBlink_ = false;
            nextBlink_ = gap(rng_);
        } else if (std::bernoulli_distribution(0.22)(rng_)) {
            secondBlink_ = true;
            nextBlink_ = 0.12f;                // counted from the end of this blink
        } else {
            nextBlink_ = gap(rng_);
        }
    }
}

const char* Mascot::Face() const {
    switch (state_) {
    case State::Music:
        if (talk_ == Talk::Speaking) return mouthOpen_ ? "happy" : "neutral";    // talking: the mouth opens and closes
        return "happy";
    case State::Notify: return "surprised";
    case State::Permission: return "wink";
    default:
        if (talk_ == Talk::Listening) return "surprised";                         // wide, attentive eyes
        return blinkLeft_ > 0 ? "blink" : "neutral";
    }
}

void Mascot::Sample(const Playback& p, std::vector<MascotAsset::TRS>& pose) const {
    if (p.clip < 0) return;
    for (const auto& ch : asset_->clips[p.clip].channels) {
        if (ch.node < 0) continue;
        MascotAsset::TRS& trs = pose[ch.node];
        switch (ch.path) {
        case MascotAsset::Channel::Path::Translation: SampleChannel(ch, p.t, &trs.t.x, 3); break;
        case MascotAsset::Channel::Path::Rotation: SampleChannel(ch, p.t, &trs.r.x, 4); break;
        case MascotAsset::Channel::Path::Scale: SampleChannel(ch, p.t, &trs.s.x, 3); break;
        }
    }
}

void Mascot::ComputeSkin(std::vector<XMFLOAT4X4>& out) const {
    const auto& nodes = asset_->nodes;
    std::vector<MascotAsset::TRS> pose(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) pose[i] = nodes[i].rest;
    Sample(current_, pose);

    if (blend_ < 1.0f && previous_.clip >= 0) {
        std::vector<MascotAsset::TRS> from(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) from[i] = nodes[i].rest;
        Sample(previous_, from);
        const float w = blend_ * blend_ * (3 - 2 * blend_);   // ease the cross-fade
        for (size_t i = 0; i < nodes.size(); ++i) {
            XMStoreFloat3(&pose[i].t, XMVectorLerp(XMLoadFloat3(&from[i].t), XMLoadFloat3(&pose[i].t), w));
            XMStoreFloat3(&pose[i].s, XMVectorLerp(XMLoadFloat3(&from[i].s), XMLoadFloat3(&pose[i].s), w));
            XMStoreFloat4(&pose[i].r, XMQuaternionSlerp(XMLoadFloat4(&from[i].r), XMLoadFloat4(&pose[i].r), w));
        }
    }

    // World matrices; parents may come after children in glTF order, so resolve recursively.
    std::vector<XMFLOAT4X4> world(nodes.size());
    std::vector<char> done(nodes.size(), 0);
    auto resolve = [&](auto&& self, int i) -> XMMATRIX {
        if (done[i]) return XMLoadFloat4x4(&world[i]);
        XMMATRIX m = ToMatrix(pose[i]);
        if (nodes[i].parent >= 0) m = m * self(self, nodes[i].parent);
        XMStoreFloat4x4(&world[i], m);
        done[i] = 1;
        return m;
    };

    out.resize(asset_->joints.size());
    for (size_t j = 0; j < asset_->joints.size(); ++j) {
        const XMMATRIX jointWorld = resolve(resolve, asset_->joints[j]);
        XMStoreFloat4x4(&out[j], XMLoadFloat4x4(&asset_->inverseBind[j]) * jointWorld);
    }
}

} // namespace eflow
