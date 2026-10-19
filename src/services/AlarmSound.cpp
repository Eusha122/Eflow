#include "services/AlarmSound.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eflow {
namespace {

constexpr int kRate = 44100;

// A soft bell: a sine with a little of its octave, a quick rise and a long fade.
void Note(std::vector<float>& out, double start, double freq, double length, float volume) {
    const size_t first = static_cast<size_t>(start * kRate), count = static_cast<size_t>(length * kRate);
    if (out.size() < first + count) out.resize(first + count, 0.0f);
    for (size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double envelope = std::min(1.0, t / 0.006) * std::exp(-t * 3.2);
        const double wave = std::sin(2 * 3.14159265358979 * freq * t) + 0.18 * std::sin(2 * 3.14159265358979 * freq * 2 * t);
        out[first + i] += static_cast<float>(volume * envelope * wave);
    }
}

std::vector<uint8_t> Wav(const std::vector<float>& samples) {
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
    std::vector<uint8_t> wav(44 + dataBytes);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);                                      // PCM
    put16(22, 1);                                      // mono
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    put32(40, dataBytes);
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto s = static_cast<int16_t>(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32000.0f));
        std::memcpy(&wav[44 + i * 2], &s, 2);
    }
    return wav;
}

}  // namespace

AlarmSound::AlarmSound() {
    std::vector<float> ring;                           // C6 E6 G6, then a pause: about 2.4 s per round
    Note(ring, 0.00, 1046.5, 1.2, 0.30f);
    Note(ring, 0.18, 1318.5, 1.2, 0.26f);
    Note(ring, 0.36, 1568.0, 1.4, 0.24f);
    ring.resize(static_cast<size_t>(2.4 * kRate), 0.0f);
    ring_ = Wav(ring);
    std::vector<float> heads;                          // G5 C6, once
    Note(heads, 0.00, 784.0, 1.0, 0.22f);
    Note(heads, 0.16, 1046.5, 1.2, 0.20f);
    headsUp_ = Wav(heads);
}

void AlarmSound::Ring() {
    PlaySoundW(reinterpret_cast<LPCWSTR>(ring_.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
}

void AlarmSound::HeadsUp() {
    PlaySoundW(reinterpret_cast<LPCWSTR>(headsUp_.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void AlarmSound::Stop() { PlaySoundW(nullptr, nullptr, 0); }

} // namespace eflow
