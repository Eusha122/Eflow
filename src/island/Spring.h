#pragma once
#include <algorithm>
#include <cmath>

namespace eflow {

// Damped harmonic oscillator, integrated with fixed sub-steps so it stays stable at any frame time.
struct Spring {
    float x = 0, v = 0, target = 0;
    float stiffness = 260, damping = 24;

    void Snap(float value) { x = target = value; v = 0; }

    void Step(float dt) {
        constexpr float h = 1.0f / 240.0f;
        dt = std::min(dt, 0.1f);
        for (float t = 0; t < dt; t += h) {
            const float s = std::min(h, dt - t);
            v += (stiffness * (target - x) - damping * v) * s;
            x += v * s;
        }
    }

    bool AtRest(float eps = 0.04f) const { return std::fabs(x - target) < eps && std::fabs(v) < eps; }
};

// Exponential approach for small state (hover/press amounts); frame-rate independent.
inline bool Approach(float& value, float target, float dt, float rate = 18.0f) {
    value += (target - value) * (1.0f - std::exp(-rate * dt));
    if (std::fabs(target - value) < 0.002f) { value = target; return false; }
    return true;
}

inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float SmoothStep(float a, float b, float v) {
    const float t = Clamp01((v - a) / (b - a));
    return t * t * (3 - 2 * t);
}

} // namespace eflow
