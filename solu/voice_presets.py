"""Solu voice modes: blends of Kokoro voices + pitch lift + pace, rendered to samples you can listen to.

The same preset table is what the settings picker on the island will list (it is saved as presets.json).
Pitch lift = render slower, then resample faster: pitch AND formants rise together, which is what makes
a voice sound smaller / more like a little creature instead of just a higher-pitched adult.
"""
import json
import time

import numpy as np
import soundfile as sf
from kokoro_onnx import Kokoro

import config

# name -> (voice blend {voice: weight}, pitch ratio, pace, description)
PRESETS = {
    "cozy":   ({"af_heart": 0.6, "af_sky": 0.4},       1.10, 1.00, "Warm and gentle, like a purring friend"),
    "bubbly": ({"af_bella": 0.5, "af_sky": 0.5},       1.18, 1.08, "Bright and excited, quick little voice"),
    "tiny":   ({"af_nicole": 0.5, "af_sarah": 0.5},    1.28, 1.05, "Small and squeaky-sweet, the cutest"),
    "calm":   ({"bf_lily": 0.6, "af_heart": 0.4},      1.08, 0.94, "Soft and unhurried, good for late evenings"),
    "natural": ({"af_heart": 1.0},                     1.00, 1.00, "No pet effect, just the natural voice"),
}
LINES = [
    "Oh, hi! I was just curled up near your keyboard.",
    "Hmm, I'm not totally sure about that one, but let's figure it out together!",
    "Yay, all done! Want me to open your music?",
]


def load_voice(kokoro, blend):
    mixed = sum(kokoro.get_voice_style(v) * w for v, w in blend.items())
    return mixed.astype(np.float32)


def speak(kokoro, preset, text):
    blend, pitch, pace, _ = PRESETS[preset]
    audio, sr = kokoro.create(text, voice=load_voice(kokoro, blend), speed=pace / pitch, lang="en-us")
    if pitch != 1.0:                                              # resample: faster = higher pitch + smaller voice
        n = int(len(audio) / pitch)
        audio = np.interp(np.linspace(0, len(audio) - 1, n), np.arange(len(audio)), audio).astype(np.float32)
    return audio, sr


def f0(audio, sr):
    """Median voiced pitch in Hz, by autocorrelation over 40 ms frames."""
    hop, size, vals = int(sr * 0.02), int(sr * 0.04), []
    lo, hi = int(sr / 500), int(sr / 100)
    for i in range(0, len(audio) - size, hop):
        x = audio[i:i + size] - audio[i:i + size].mean()
        if np.sqrt((x ** 2).mean()) < 0.02:
            continue
        ac = np.correlate(x, x, "full")[size - 1:]
        lag = lo + int(np.argmax(ac[lo:hi]))
        if ac[lag] > 0.5 * ac[0]:
            vals.append(sr / lag)
    return float(np.median(vals)) if vals else 0.0


if __name__ == "__main__":
    k = Kokoro(config.model("kokoro-v1.0.onnx"), config.model("voices-v1.0.bin"))
    speak(k, "natural", "warm up")
    for name, (_, pitch, pace, desc) in PRESETS.items():
        t = time.perf_counter()
        parts = [speak(k, name, line) for line in LINES]
        took = time.perf_counter() - t
        sr = parts[0][1]
        audio = np.concatenate([np.concatenate([a, np.zeros(int(sr * 0.3), np.float32)]) for a, _ in parts])
        sf.write(f"voice_{name}.wav", audio, sr)
        print(f"{name:8s} pitch ~{f0(audio, sr):4.0f} Hz | {len(audio)/sr:4.1f}s audio in {took:.1f}s | {desc}")
    json.dump({n: {"voices": b, "pitch": p, "pace": s, "description": d} for n, (b, p, s, d) in PRESETS.items()},
              open("presets.json", "w"), indent=2)
