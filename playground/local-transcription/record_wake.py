#!/usr/bin/env python3
"""record_wake.py — record a TIGHT reference set of the wake phrase.

Quality matters more than count. Say the phrase the SAME way every take,
same speed, same intonation — no pauses in the middle. 6 consistent takes
give the detector a tight prototype so it fires ONLY on this phrase.

Usage:  .venv/bin/python record_wake.py [out_dir] [phrase]
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sounddevice as sd
import soundfile as sf
import numpy as np
import glob

from voice import _resolve_input_device, _native_rate

OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "wake-refs"
PHRASE = sys.argv[2] if len(sys.argv) > 2 else "morpheus"
N = int(sys.argv[3]) if len(sys.argv) > 3 else 6
OUT.mkdir(parents=True, exist_ok=True)

# clear previous refs for THIS phrase so we get a fresh, consistent set
for old in glob.glob(str(OUT / f"{PHRASE}-*.wav")):
    try:
        Path(old).unlink()
    except Exception:
        pass

dev = _resolve_input_device("USB Composite Device Mono")
native_sr = int(_native_rate(dev))
SR = 16000
SECS = 2.0
TARGET = int(SECS * SR)
GAIN = float(sys.argv[4]) if len(sys.argv) > 4 else 4.5  # software gain (clipping-free)

def _resample(x, from_rate):
    if from_rate == SR:
        return x
    n = int(len(x) * SR / from_rate)
    idx = np.clip((np.arange(n) * from_rate / SR).astype(int), 0, len(x) - 1)
    return x[idx]

def _speech_dur(x):
    # fraction of the clip that is above a quiet floor (estimate of speech)
    return float((np.abs(x) > 0.03).mean())

print(f"[record] mic={dev} native@{native_sr}Hz → {SR}Hz → {OUT}")
print(f"[record] {N} takes. Say \"{PHRASE}\" the SAME way each time — same speed, "
      f"same tone, in the middle of the 2s window, then be quiet.\n")
durs = []
for i in range(N):
    input(f"→ take {i+1}/{N}: say \"{PHRASE}\" now")
    x = sd.rec(int(SECS * native_sr), samplerate=native_sr, channels=1, dtype="float32", device=dev)
    sd.wait()
    x = _resample(x[:, 0], native_sr)[:TARGET]
    # Software gain: the USB mic is quiet (~0.05-0.1 peak even close up). Boost
    # so speech sits at a healthy level for VAD / wake detection.
    x = np.clip(x * GAIN, -1.0, 1.0)
    peak = float(np.abs(x).max())
    frac = _speech_dur(x)
    ok = peak > 0.05 and 0.03 < frac < 0.5
    durs.append(frac)
    path = OUT / f"{PHRASE}-{i+1}.wav"
    sf.write(str(path), x, SR)
    note = "ok" if ok else "REDO — empty, too long, or quiet"
    print(f"  saved {path.name} (peak {peak:.3f}, speech {frac*100:.0f}%) [{note}]")
print(f"\n[done] {N} takes in {OUT}. Speak {PHRASE} the same way live to wake.")
