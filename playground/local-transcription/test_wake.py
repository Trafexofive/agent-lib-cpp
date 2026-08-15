#!/usr/bin/env python3
"""test_wake.py — diagnostic: does the LIVE audio path produce a recognizable wake?

Captures through the SAME processing the backend uses (native rate → resample →
gain → Vosk), feeds it to Vosk, and reports if "morpheus" is recognized. Also
saves the capture so we can inspect exactly what the mic heard.

Usage:  .venv/bin/python test_wake.py [seconds]
Say "morpheus" clearly during the capture window, then stay quiet.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np
import sounddevice as sd
import soundfile as sf

from voice import _resolve_input_device, _native_rate, VoskWakeDetector, SAMPLE_RATE

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
GAIN = 4.5

dev = _resolve_input_device("USB Composite Device Mono")
sr = int(_native_rate(dev))
print(f"[test] mic={dev} @{sr}Hz, {SECS}s window, gain {GAIN}x")
print(f"[test] SAY \"MORPHEUS\" during the window, then be quiet…")
x = sd.rec(int(SECS * sr), samplerate=sr, channels=1, dtype="float32", device=dev)
sd.wait()
x = x[:, 0]

# exact live path: gain on native audio, then resample to 16k
x = np.clip(x * GAIN, -1.0, 1.0)
n = int(len(x) * SAMPLE_RATE / sr)
idx = np.clip((np.arange(n) * sr / SAMPLE_RATE).astype(int), 0, len(x) - 1)
x = x[idx]

sf.write("/tmp/live_morpheus_test.wav", x, SAMPLE_RATE)

d = VoskWakeDetector("wake-model/vosk-small-en", keyword="morpheus")
fired = False
for i in range(0, len(x) - 1280 + 1, 1280):
    if d.feed(x[i:i + 1280]):
        fired = True
        break
if len(x) >= 1280:
    d.feed(x[-1280:])
fired = fired or d.flush()

print(f"[test] raw peak={np.abs(x).max():.3f}")
print(f"[test] Vosk recognized \"morpheus\": {'FIRES ✓' if fired else 'MISS ✗'}")
print("[test] saved /tmp/live_morpheus_test.wav — say the word and report FIRES/MISS")
