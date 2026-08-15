#!/usr/bin/env python3
"""test_stream.py — isolates the backend's InputStream path.

Replicates voice.py's exact mic callback (native rate → gain → resample → ring),
then feeds the captured audio to Vosk. Tells us whether the InputStream path
produces recognizable wake audio (vs test_wake.py's one-shot sd.rec).

Say "morpheus" after it starts, then press Enter.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np
import sounddevice as sd
from collections import deque
import soundfile as sf

from voice import _resolve_input_device, _native_rate, VoskWakeDetector, SAMPLE_RATE

GAIN = 4.5
WAKE_CHUNK = 1280
dev = _resolve_input_device("USB Composite Device Mono")
mic_rate = int(_native_rate(dev))

ring = deque()
carry = np.zeros(0, dtype=np.float32)

def cb(indata, frames, t, status):
    global carry
    x = indata[:, 0]
    x = np.clip(x * GAIN, -1.0, 1.0)
    if mic_rate != SAMPLE_RATE and len(x):
        n = max(1, int(len(x) * SAMPLE_RATE / mic_rate))
        idx = np.arange(n) * mic_rate / SAMPLE_RATE
        idx = np.clip(idx.astype(int), 0, len(x) - 1)
        x = x[idx]
    carry = np.concatenate([carry, x])
    n = len(carry) // WAKE_CHUNK
    for i in range(n):
        ring.append(carry[i * WAKE_CHUNK:(i + 1) * WAKE_CHUNK])
    carry = carry[n * WAKE_CHUNK:]

stream = sd.InputStream(samplerate=mic_rate, blocksize=int(mic_rate * 0.08),
                        channels=1, dtype="float32", device=dev, callback=cb)
stream.start()
print(f"[stream] mic={dev} @{mic_rate}Hz. SAY \"MORPHEUS\" now, then press Enter.")
input()
stream.stop(); stream.close()

audio = np.concatenate(list(ring) + ([carry] if carry.size else [])) if (ring or carry.size) else np.zeros(0)
print(f"[stream] captured {len(audio)/SAMPLE_RATE:.2f}s, peak={np.abs(audio).max():.3f}")
sf.write("/tmp/stream_morpheus_test.wav", audio, SAMPLE_RATE)

d = VoskWakeDetector("wake-model/vosk-small-en", keyword="morpheus")
fired = False
for i in range(0, len(audio) - 1280 + 1, 1280):
    if d.feed(audio[i:i + 1280]):
        fired = True
        break
print(f"[stream] Vosk recognized \"morpheus\": {'FIRES ✓' if fired else 'MISS ✗'}")
print("[stream] saved /tmp/stream_morpheus_test.wav — report FIRES/MISS")
