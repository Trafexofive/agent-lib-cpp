#!/usr/bin/env python3
"""
voice.py — wake word → STT → ephemeral harness session → response → TTS.

Latency-first playground pipeline for Cortex-Prime MK3:
    wake word (openwakeword) → VAD endpoint (silero) → transcription
    (faster-whisper tiny.en on CUDA) → `cortex-mk3 -m voice run -p "<text>"
    --ephemeral --no-ansi` (headless, streams rendered blocks) → final
    [response] block → speech (kokoro, streamed playback).

Every engine is warmed at startup. The only cold part per utterance is the
ephemeral harness spawn (~0.5s) + the LLM turn — that is the dominant cost.

Per-stage latency is logged to stderr. Ctrl-C exits cleanly.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path
from threading import Lock

import numpy as np

# ── engine imports (heavy; imported after arg parse so --help stays fast) ──

SAMPLE_RATE = 16000
WAKE_CHUNK = 1280  # 80 ms @ 16 kHz (openwakeword contract)
VAD_CHUNK = 512    # 32 ms @ 16 kHz (silero v4 contract)
TTS_RATE = 24000


def _as_device(x):
    """sounddevice accepts int index or string name; a bare '22' is a name,
    not an index — coerce numeric strings so `--device 22` selects device 22."""
    if x is None:
        return None
    try:
        return int(x)
    except (TypeError, ValueError):
        return x


def _resolve_input_device(arg):
    """Resolve a device arg to a sounddevice input device index.

    arg can be: None (auto → prefer a USB/composite mic, else default),
    an int index, a numeric string, or a name substring (case-insensitive).
    Indices shift when devices come/go (AudioRelay, HDMI, …), so name-matching
    is the stable choice.
    """
    import sounddevice as sd

    if arg is None:
        # Auto-detect: prefer a USB/composite mic, but only if it actually has
        # live audio (not digital silence — e.g. a standby/unpowered interface).
        # Fall back to any input with a real signal, then the default.
        try:
            cands = [i for i, d in enumerate(sd.query_devices())
                     if d["max_input_channels"] > 0 and "USB" in d["name"] and "Composite" in d["name"]]
            for i in cands:
                if _has_signal(i, seconds=0.4):
                    return i
        except Exception:
            pass
        try:
            for i, d in enumerate(sd.query_devices()):
                if d["max_input_channels"] > 0 and _has_signal(i, seconds=0.4):
                    return i
        except Exception:
            pass
        return None
    if isinstance(arg, int):
        return arg
    s = str(arg)
    if s.isdigit():
        return int(s)
    s_l = s.lower()
    try:
        for i, d in enumerate(sd.query_devices()):
            if s_l in d["name"].lower() and d["max_input_channels"] > 0:
                return i
    except Exception:
        pass
    return s  # let sounddevice try by exact name


def _has_signal(dev, seconds=0.4) -> bool:
    """Return True if a device captures a real signal (non-digital-silence)."""
    import sounddevice as sd

    try:
        d = sd.query_devices(dev)
        sr = int(d.get("default_samplerate") or SAMPLE_RATE)
        x = sd.rec(int(seconds * sr), samplerate=sr, channels=1, dtype="float32", device=dev)
        sd.wait()
        x = x[:, 0] if x.ndim > 1 else x
        return float(np.abs(x).max()) > 1e-4
    except Exception:
        return False


def _native_rate(dev):
    """default_samplerate of a device index, or 16000 if unknown."""
    import sounddevice as sd

    try:
        if isinstance(dev, int):
            return int(sd.query_devices(dev)["default_samplerate"])
    except Exception:
        pass
    return SAMPLE_RATE

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO_ROOT / "playground/local-transcription/manifests/agents/voice/agent.yml"
DEFAULT_BIN = REPO_ROOT / "cortex-mk3"
SILERO_MODEL = Path(__file__).resolve().parent / "silero_vad.onnx"


class Clock:
    """Per-stage latency logger (stderr, one line per utterance)."""

    def __init__(self) -> None:
        self.t0 = time.perf_counter()
        self.marks: dict[str, float] = {}

    def mark(self, name: str) -> None:
        self.marks[name] = time.perf_counter()

    def deltas_ms(self) -> dict[str, int]:
        """Sequential per-stage deltas (ms) for the console state file."""
        out: dict[str, int] = {}
        base = self.t0
        for name, t in self.marks.items():
            out[name] = max(0, int(1000 * (t - base)))
            base = t
        out["total"] = max(0, int(1000 * (time.perf_counter() - self.t0)))
        return out

    def report(self) -> None:
        parts = [f"{k}={v}ms" for k, v in self.deltas_ms().items()]
        print(f"[voice] {' '.join(parts)}", file=sys.stderr)


class Ring:
    """Locked audio ring buffer fed by the sounddevice callback."""

    def __init__(self, max_frames: int = 400) -> None:
        self._q: deque[np.ndarray] = deque(maxlen=max_frames)
        self._lock = Lock()

    def push(self, frame: np.ndarray) -> None:
        with self._lock:
            self._q.append(frame.copy())

    def pop(self) -> np.ndarray | None:
        with self._lock:
            return self._q.popleft() if self._q else None

    def drain(self) -> np.ndarray:
        with self._lock:
            frames = list(self._q)
            self._q.clear()
        if not frames:
            return np.zeros(0, dtype=np.float32)
        return np.concatenate(frames)


class SileroVAD:
    """Speech detection wrapping faster-whisper's bundled silero VAD.

    The raw silero_vad.onnx from upstream needs an h/c/context input contract;
    calling it the classic state-only way silently returns ~0 for everything.
    faster-whisper bundles the same model already wired correctly.
    """

    def __init__(self) -> None:
        from faster_whisper.vad import get_speech_timestamps, get_vad_model

        self._model = get_vad_model()
        self._get_ts = get_speech_timestamps

    def has_speech(self, audio: np.ndarray) -> bool:
        return bool(self.speech_segments(audio))

    def speech_segments(self, audio: np.ndarray) -> list:
        """list of {'start':s,'end':s} speech segments (samples) or []."""
        try:
            return self._get_ts(audio, sampling_rate=SAMPLE_RATE)
        except Exception:
            return []

    def reset(self) -> None:
        pass


class WhisperWakeDetector:
    """Wake-word detection via the (verified) whisper STT.

    openwakeword 0.6.0 is unusable here (ONNX path scores ~0 on everything;
    tflite-runtime has no py3.14 wheel). Instead, continuously transcribe a
    rolling ~2 s window with whisper tiny.en (GPU, ~20 ms) and trigger when
    the transcript contains the wake phrase. Fully local, zero accounts.
    """

    def __init__(self, whisper, vad, phrase: str = "alexa", poll_s: float = 0.25) -> None:
        self.whisper = whisper
        self.vad = vad
        self.phrase = phrase.strip().lower()
        self.poll_s = poll_s
        self.buf: np.ndarray = np.zeros(0, dtype=np.float32)
        self.last_poll = 0.0

    def feed(self, frame: np.ndarray, now: float) -> bool:
        self.buf = np.concatenate([self.buf, frame])
        limit = int(SAMPLE_RATE * 2.0)
        if self.buf.size > limit:
            self.buf = self.buf[-limit:]
        # transcribe at most every poll_s; need at least ~0.8 s to hear a word
        if now - self.last_poll < self.poll_s or self.buf.size < int(SAMPLE_RATE * 0.8):
            return False
        self.last_poll = now
        # VAD gate: never transcribe silence/quiet noise — whisper tiny.en
        # hallucinates short words ("alexa") on ambient room tone, which
        # caused constant false wake triggers.
        if not self.vad.has_speech(self.buf):
            return False
        try:
            segs, info = self.whisper.transcribe(self.buf, beam_size=1, language="en")
            if getattr(info, "no_speech_prob", 0.0) > 0.5:
                return False
            text = "".join(s.text for s in segs).lower()
            # normalize to words only
            text = re.sub(r"[^a-z' ]", " ", text)
            text = re.sub(r"\s+", " ", text).strip()
            # strip leading filler that often precedes a wake address
            text = re.sub(r"^(hey|ok|okay|please)\b\s*", "", text)
            # ISOLATION: fire only when the wake phrase STARTS the utterance.
            # This catches "alexa" and "alexa <command>", but rejects a
            # hallucinated/similar-sounding "alexa" buried mid-sentence in
            # normal conversation — which was causing replies to non-wakes.
            if not text.startswith(self.phrase):
                return False
            # confidence gate on the matched (first) segment
            segs = list(segs)
            if not segs:
                return False
            seg = segs[0]
            if seg.no_speech_prob > 0.4 or seg.avg_logprob < -1.2:
                return False
            # isolation: wake word is a short isolated burst, not continuous
            # audio dominating the whole 2s buffer
            vs = self.vad.speech_segments(self.buf)
            total = sum(int(s["end"] - s["start"]) for s in vs)
            buf_samples = len(self.buf)
            if buf_samples > 0 and total / buf_samples > 0.7:
                return False
        except Exception:
            return False
        return True


class OpenWakewordDetector:
    """Wake-word detection via openwakeword (free, Apache-2.0, no key).

    Canonical streaming: ONE persistent model, feed 80 ms frames; the
    preprocessor accumulates speech embeddings internally and each call scores
    the current window. Returns the max score across the default models.
    """

    def __init__(self, threshold: float = 0.5) -> None:
        from openwakeword import Model as WakeModel

        self.oww = WakeModel(inference_framework="onnx")
        self.threshold = threshold

    def score(self, frame: np.ndarray) -> tuple[float, str]:
        """(max_score, best_model) for this 1280-sample frame."""
        try:
            scores = self.oww.predict(frame)
        except Exception:
            return 0.0, ""
        if not scores:
            return 0.0, ""
        best = max(scores, key=scores.get)
        return float(scores[best]), best

    def is_wake(self, frame: np.ndarray) -> bool:
        s, _ = self.score(frame)
        return s > self.threshold


class VoskWakeDetector:
    """Wake-word detection via Vosk keyword spotting (free, Apache-2.0, no key).

    Scan-based: audio accumulates into a rolling buffer; every SCAN samples a
    FRESH KaldiRecognizer runs on the buffer (the reliable batch path) and
    checks whether the first recognized word is the wake word. More robust
    than continuous streaming recognition, which struggles in live/ambient
    audio. Fires only when the actual word is spoken.
    """

    def __init__(self, model_path: str, keyword: str = "morpheus",
                 scan_sec: float = 0.4, buffer_sec: float = 2.0) -> None:
        import json as _json

        from vosk import KaldiRecognizer, Model

        self.keyword = keyword.lower()
        self.model = Model(model_path)
        self._KaldiRecognizer = KaldiRecognizer
        self.grammar = _json.dumps([keyword, "[unk]"])
        self._buf = np.zeros(0, dtype=np.float32)
        self._scan_n = int(scan_sec * SAMPLE_RATE)
        self._max = int(buffer_sec * SAMPLE_RATE)
        self._since = 0

    def feed(self, frame: np.ndarray) -> bool:
        """Accumulate; every SCAN samples run a fresh batch scan for the wake."""
        self._buf = np.concatenate([self._buf, frame])
        if self._buf.size > self._max:
            self._buf = self._buf[-self._max:]
        self._since += len(frame)
        if self._since < self._scan_n:
            return False
        self._since = 0
        return self._scan()

    def _scan(self) -> bool:
        buf = self._buf
        pk = float(np.abs(buf).max()) if len(buf) else 0.0
        if pk > 0.01:
            buf = buf * (0.7 / pk)  # volume-normalize
        rec = self._KaldiRecognizer(self.model, SAMPLE_RATE, self.grammar)
        x = (buf * 32767.0).astype(np.int16)
        for i in range(0, len(x) - 3200 + 1, 3200):
            if rec.AcceptWaveform(x[i:i + 3200].tobytes()):
                r = rec.Result()
                self._scan_result = r
                if self._match(r):
                    return True
        r = rec.FinalResult()
        self._scan_result = r
        return self._match(r)

    def _match(self, result: str) -> bool:
        try:
            import json as _json

            words = re.findall(r"[a-z']+", _json.loads(result).get("text", "").lower())
            # Wake word as a standalone word in a SHORT segment (<=3 words: the
            # word + breath/noise like [unk] morpheus). Rejects continuous
            # speech where "morpheus" might be hallucinated among many words.
            return self.keyword in words and len(words) <= 3
        except Exception:
            return False

    def debug_partial(self) -> str:
        """Last scan's recognized text (for --debug-wake), parsed."""
        try:
            import json as _json

            return _json.loads(getattr(self, "_scan_result", "{}")).get("text", "")
        except Exception:
            return ""

    def debug_heard(self) -> str:
        """What Vosk recognized in the firing scan (parsed text)."""
        return self.debug_partial()


class PorcupineWakeDetector:
    """Purpose-built wake word engine (like a phone's wake detector).

    Porcupine is a dedicated acoustic model that fires on ONE keyword and
    rejects everything else — not a generative STT model or a loose keyword
    matcher. Needs a free Picovoice AccessKey. The keyword is swappable at
    construction (--wake-word); built-in keywords like "terminator" work out
    of the box, custom ones need a trained .ppn.
    """

    def __init__(self, access_key: str, keyword: str = "terminator") -> None:
        import pvporcupine

        self.keyword = keyword
        self.p = pvporcupine.create(access_key=access_key, keywords=[keyword])
        self.frame_len = int(self.p.frame_length)  # 512 samples @ 16 kHz
        self.buf = np.zeros(0, dtype=np.float32)

    def feed(self, frame: np.ndarray) -> bool:
        """Accumulate 16k float32 frames; True when the keyword is detected.

        Porcupine expects exact frame_length chunks, so we buffer the stream
        and hand it frame_len samples at a time.
        """
        self.buf = np.concatenate([self.buf, frame])
        fired = False
        while self.buf.size >= self.frame_len:
            chunk = self.buf[: self.frame_len]
            self.buf = self.buf[self.frame_len :]
            data = (chunk * 32767.0).astype(np.int16)
            if self.p.process(data) >= 0:
                fired = True
        return fired


class LocalWakeDetector:
    """Offline, no-key, no-training wake word via ONNX speech-embedding + DTW.

    Template-matching against the operator's own recorded reference samples of
    the wake phrase (local-wake's detection core: librosa + onnxruntime only).
    Fully local — no network, no account, no model training. Swap the phrase
    by recording a different reference set.
    """

    def __init__(self, ref_folder: str, model_path: str, threshold: float = 0.07,
                 buffer_sec: float = 2.0, slide_sec: float = 0.25, vad=None) -> None:
        import librosa
        import onnxruntime as ort

        self._librosa = librosa
        self.threshold = threshold
        self._vad = vad
        self._sess = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
        self.refs: list[np.ndarray] = []
        for f in sorted(os.listdir(ref_folder)):
            if f.lower().endswith(".wav"):
                y, _ = librosa.load(os.path.join(ref_folder, f), sr=16000)
                y = self._pad1(self._trim(y))  # speech-only, then fixed 1s for the model
                if len(y) < int(0.3 * 16000):
                    continue
                self.refs.append(self._embed(y))
        if not self.refs:
            raise RuntimeError(f"no usable reference samples in {ref_folder} — record the wake phrase first")
        self.buffer = np.zeros(int(buffer_sec * 16000), dtype=np.float32)
        self._slide_n = int(slide_sec * 16000)
        self._pending = 0

    @staticmethod
    def _trim(y: np.ndarray, sr: int = 16000, thresh: float = 0.02,
              pad_ms: int = 200) -> np.ndarray:
        """Trim leading/trailing silence to the speech region."""
        win, hop = int(0.02 * sr), int(0.01 * sr)
        n = 1 + (len(y) - win) // hop
        env = np.abs(y[: win + (n - 1) * hop].reshape(-1, hop)[:n, :win]).max(axis=1)
        idx = np.where(env > thresh)[0]
        if len(idx) == 0:
            return y
        s = max(0, idx[0] * hop - int(pad_ms * sr / 1000))
        t = min(len(y), (idx[-1] + 1) * hop + int(pad_ms * sr / 1000))
        return y[s:t]

    @staticmethod
    def _pad1(y: np.ndarray) -> np.ndarray:
        """Pad/truncate to the model's minimum 1 s (16000 samples)."""
        if len(y) < 16000:
            return np.pad(y, (0, 16000 - len(y)))
        return y[:16000]

    def _embed(self, y: np.ndarray) -> np.ndarray:
        y = np.asarray(y, dtype=np.float32)
        if y.ndim == 1:
            y = y[None, :]
        out = self._sess.run(None, {"samples:0": y})[0]
        return out[0, :, 0, :].T  # (embed_dim, time_frames)

    def feed(self, frame: np.ndarray) -> bool:
        self.buffer = np.roll(self.buffer, -len(frame))
        self.buffer[-len(frame):] = frame
        self._pending += len(frame)
        if self._pending < self._slide_n:
            return False
        self._pending = 0
        # Isolation gate: the wake word must be a SINGLE short utterance bounded
        # by silence (VAD speech segment ~0.3-0.9s). Continuous speech / a long
        # sentence produces a long segment -> never a wake word -> no fire.
        if self._vad is None:
            seg = self._pad1(self._trim(self.buffer))
            if len(self._trim(self.buffer)) < int(0.3 * 16000):
                return False
            return min(self._dtw(self._embed(seg), r) for r in self.refs) < self.threshold
        segs = self._vad.speech_segments(self.buffer)
        if not segs:
            return False
        last = segs[-1]
        dur = (last["end"] - last["start"]) / 16000
        if not (0.25 <= dur <= 0.9):
            return False
        y = self.buffer[int(last["start"]): int(last["end"])]
        if len(y) < int(0.25 * 16000):
            return False
        emb = self._embed(self._pad1(y))
        return min(self._dtw(emb, r) for r in self.refs) < self.threshold

    def _dtw(self, a: np.ndarray, b: np.ndarray) -> float:
        cost, _ = self._librosa.sequence.dtw(X=a, Y=b, metric="cosine")
        return float(cost[-1, -1] / (a.shape[1] + b.shape[1]))


def extract_response(stdout: str) -> str:
    """Grab the final answer from headless --no-ansi output.

    Prefer the last [response] block. When the model (thinking on) emits its
    answer as a final <thought> with no <response> tag, fall back to the last
    [thought] block so we never drop a valid answer to "empty".
    """
    resp: list[str] = []
    last_thought = ""
    cur: list[str] | None = None
    cur_kind = ""
    for line in stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if cur is not None:
                t = "\n".join(cur).strip()
                if t:
                    if cur_kind == "response":
                        resp.append(t)
                    elif cur_kind == "thought":
                        last_thought = t
            cur = None
            cur_kind = ""
            if stripped == "[response]":
                cur, cur_kind = [], "response"
            elif stripped == "[thought]":
                cur, cur_kind = [], "thought"
            continue
        if cur is not None:
            cur.append(line)
    if cur is not None:
        t = "\n".join(cur).strip()
        if t:
            if cur_kind == "response":
                resp.append(t)
            elif cur_kind == "thought":
                last_thought = t
    text = resp[-1] if resp else last_thought
    if not text:
        return ""
    # Belt-and-braces cleanup: strip any markdown leftovers.
    text = re.sub(r"```[a-z]*\n?", "", text)
    text = re.sub(r"[*_`>#]", "", text)
    return re.sub(r"\s+", " ", text).strip()


class VoiceLoop:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        import sounddevice as sd

        self.sd = sd

        # Console state / lifecycle. Set up FIRST so the TUI sees a live status
        # immediately ("starting…") instead of an empty screen while models load.
        self.stage = "starting"
        self.status = "starting…"
        self.transcript = ""
        self.response = ""
        self.state_path: Path | None = args.state_out
        self.clock = Clock()
        self.level = 0.0
        self.wake_peak = 0.0
        self.wake_model = ""
        # PID file so run.sh / the TUI can kill THIS exact process deterministically
        # instead of pkill-guessing (which let stale zombies survive).
        if args.state_out:
            try:
                Path(str(args.state_out) + ".pid").write_text(str(os.getpid()))
            except Exception:
                pass
        self.write_state()

        # ── STT (loaded first — the wake detector reuses it) ──
        from faster_whisper import WhisperModel

        self.status = "loading whisper (CUDA)…"
        self.write_state()
        print(f"[voice] loading whisper {args.whisper} (CUDA)…", file=sys.stderr)
        self.whisper = WhisperModel(args.whisper, device="cuda", compute_type="float16")

        # ── VAD (loaded before wake — the wake detector gates on it) ──
        self.status = "loading VAD…"
        self.write_state()
        print("[voice] loading silero VAD…", file=sys.stderr)
        self.vad = SileroVAD()

        # ── wake (Vosk keyword spotting: word-level, fires only on the phrase) ──
        self.status = "loading wake detector…"
        self.write_state()
        print(f"[voice] wake detector (Vosk): say \"{args.wake_word}\"…", file=sys.stderr)
        self.wake = VoskWakeDetector(args.vosk_model, keyword=args.wake_word)

        # ── TTS ──
        self.status = "loading kokoro TTS…"
        self.write_state()
        print("[voice] loading kokoro TTS…", file=sys.stderr)
        from kokoro import KPipeline

        try:
            self.tts = KPipeline(lang_code="a", device=args.tts_device)
        except Exception as e:
            print(f"[voice] TTS device {args.tts_device} failed ({e}); falling back to CPU", file=sys.stderr)
            self.tts = KPipeline(lang_code="a", device="cpu")
        self.voice_name = args.voice

        self.ring = Ring()
        self._stream: sd.InputStream | None = None
        self._stop = False
        self._heartbeat: threading.Thread | None = None
        self.status = "ready"
        self.stage = "idle"
        self.write_state()

    # ── console state (machine-readable JSON, atomic replace) ──
    def write_state(self, extra: dict | None = None) -> None:
        if not self.state_path:
            return
        d: dict = {
            "stage": self.stage,
            "status": getattr(self, "status", ""),
            "latency_ms": {str(k): int(v) for k, v in self.clock.deltas_ms().items()},
            "transcript": self.transcript,
            "response": self.response,
            "level": float(self.level),
            "wake_peak": float(self.wake_peak),
            "wake_model": self.wake_model,
            "wake_threshold": 0.0,
            "ts": time.time(),
        }
        if extra:
            d.update(extra)
        tmp = self.state_path.with_suffix(".tmp")
        try:
            tmp.write_text(json.dumps(d))
            tmp.replace(self.state_path)
        except OSError:
            pass

    # ── audio capture ──
    def start_mic(self) -> None:
        # Resolve by name/index; open at the device's native rate (some USB
        # mics reject 16 kHz with PortAudio -9997), resample to 16k here.
        self.mic_dev = _resolve_input_device(self.args.device)
        self.mic_rate = _native_rate(self.mic_dev)
        self._carry: np.ndarray = np.zeros(0, dtype=np.float32)
        blocksize = max(1, int(self.mic_rate * 0.08))

        def cb(indata, frames, _t, _status):
            x = indata[:, 0] if indata.ndim > 1 else indata
            # Software gain: the USB mic is quiet; boost so VAD/wake work well.
            x = np.clip(x * self.args.mic_gain, -1.0, 1.0)
            peak = float(np.abs(x).max()) if len(x) else 0.0
            self.level = max(peak, self.level * 0.75)
            # resample native → 16k if needed
            if self.mic_rate != SAMPLE_RATE and len(x):
                n = max(1, int(len(x) * SAMPLE_RATE / self.mic_rate))
                idx = np.arange(n) * self.mic_rate / SAMPLE_RATE
                idx = np.clip(idx.astype(int), 0, len(x) - 1)
                x = x[idx]
            # align to 1280-sample frames so wake/VAD stay in phase
            self._carry = np.concatenate([self._carry, x])
            n = len(self._carry) // WAKE_CHUNK
            for i in range(n):
                self.ring.push(self._carry[i * WAKE_CHUNK : (i + 1) * WAKE_CHUNK])
            self._carry = self._carry[n * WAKE_CHUNK :]

        self._stream = self.sd.InputStream(
            samplerate=self.mic_rate,
            blocksize=blocksize,
            channels=1,
            dtype="float32",
            device=self.mic_dev,
            callback=cb,
        )
        self._stream.start()
        print(f"[voice] mic: device={self.mic_dev} @ {self.mic_rate} Hz", file=sys.stderr)

    def stop_mic(self) -> None:
        if self._stream:
            self._stream.stop()
            self._stream.close()

    # ── pipeline stages ──
    def wait_wake(self, clock: Clock) -> None:
        """Block until a wake word fires; discard audio before it."""
        self.stage = "wake"
        self.wake_peak = 0.0
        self.write_state()
        last_write = time.time()
        last_dbg = time.time()
        while not self._stop:
            now = time.time()
            frame = self.ring.pop()
            if frame is None:
                time.sleep(0.002)
                continue
            # Silence guard: a real wake word can't come from digital silence.
            # Skip near-zero frames so wake models can't hallucinate "morpheus"
            # on a muted/standby mic's zeros (this caused false triggers).
            if float(np.abs(frame).max()) < 1e-4:
                continue
            fired = self.wake.feed(frame)
            if fired:
                if getattr(self.args, "debug_wake", False):
                    print(f"[WAKE] fired on peak={float(np.abs(frame).max()):.3f} vosk_heard={self.wake.debug_heard()}",
                          file=sys.stderr)
                # Distinct, visible WAKE DETECTED state before we start listening.
                self.stage = "waked"
                self.write_state({"wake_word": self.args.wake_word})
                time.sleep(0.5)  # hold so the TUI shows it, not a flash
                clock.mark("wake")
                return
            if getattr(self.args, "debug_wake", False):
                pk = float(np.abs(frame).max())
                # log whenever there's real audio so we see every speech frame
                if pk > 0.05 or (now - last_dbg > 0.5):
                    last_dbg = now
                    print(f"[dbg] peak={pk:.3f} fired={int(fired)} vosk={self.wake.debug_partial().strip()}",
                          file=sys.stderr)
            # Heartbeat only — refresh state so the TUI shows we're alive and
            # waiting. Do NOT return: we must keep waiting for the real wake.
            if now - last_write > 0.2:
                last_write = now
                self.write_state()

    def capture_utterance(self, clock: Clock) -> np.ndarray:
        """Record until the utterance ends: speech, then >= stop_silence of
        trailing silence (via the bundled silero VAD on the running buffer)."""
        frames: list[np.ndarray] = []
        started = time.perf_counter()
        last_check = 0.0
        while not self._stop:
            frame = self.ring.pop()
            if frame is None:
                time.sleep(0.002)
                continue
            frames.append(frame)
            now = time.perf_counter()
            if now - last_check < 0.12:
                continue
            last_check = now
            audio = np.concatenate(frames)
            segs = self.vad.speech_segments(audio)
            if not segs:
                # nothing intelligible yet; bail on total timeout only
                if time.perf_counter() - started > self.args.max_utterance:
                    break
                continue
            last_end = segs[-1]["end"]
            silence_since = (len(audio) - last_end) / SAMPLE_RATE
            if silence_since >= self.args.stop_silence:
                break  # speech, then enough quiet → utterance done
            if time.perf_counter() - started > self.args.max_utterance:
                break
        clock.mark("capture")
        self.stage = "listen"
        self.write_state()
        return np.concatenate(frames) if frames else np.zeros(0, dtype=np.float32)

    def transcribe(self, audio: np.ndarray, clock: Clock) -> str:
        segs, _info = self.whisper.transcribe(audio, beam_size=1, language="en")
        text = "".join(s.text for s in segs).strip()
        clock.mark("stt")
        self.stage = "stt"
        self.transcript = text
        self.write_state()
        return text

    def ask_harness(self, text: str, clock: Clock) -> str:
        cmd = [
            str(self.args.bin),
            "-m", str(self.args.manifest),
            "run", "-p", text,
            "--ephemeral", "--no-ansi",
        ]
        if self.args.provider:
            cmd = [str(self.args.bin), "--provider", self.args.provider,
                   "-m", str(self.args.manifest), "run", "-p", text,
                   "--ephemeral", "--no-ansi"]
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=self.args.llm_timeout
        )
        clock.mark("llm")
        self.stage = "llm"
        self.write_state()
        if proc.returncode != 0:
            raise RuntimeError(f"harness exit {proc.returncode}: {proc.stderr[-300:]}")
        return extract_response(proc.stdout)

    def speak(self, text: str, clock: Clock) -> None:
        """Synthesize and play; stream chunks to the output as they render."""
        out = self.sd.OutputStream(
            samplerate=TTS_RATE, channels=1, dtype="float32",
            device=_as_device(self.args.out_device)
        )
        first = True
        with out:
            for chunk in self.tts(text, voice=self.voice_name):
                if first:
                    clock.mark("tts_first_chunk")
                    first = False
                out.write(np.ascontiguousarray(chunk.audio))
        clock.mark("tts_done")
        # After speaking, the caller settles (drains + waits for quiet) before
        # re-arming wake — see _settle_after_speak. Wake stays OFF otherwise.

    # ── main loop ──
    def _heartbeat_loop(self) -> None:
        """Keep the state file fresh while idle so the console can tell
        alive-but-idle from dead."""
        while not self._stop:
            self.stage = "idle"
            self.write_state()
            for _ in range(20):  # ~2s, cancellable
                if self._stop:
                    return
                time.sleep(0.1)

    def _settle_after_speak(self, cooldown: float = 3.0, max_wait: float = 4.0,
                            quiet_s: float = 0.6, quiet_thresh: float = 0.02) -> None:
        """No-continuation barrier after a reply.

        After speaking, enforce a minimum cooldown during which ALL mic audio
        is drained (nothing is listened to), then additionally wait for the
        room to be quiet (our TTS echo to die). Only then does wake detection
        re-arm. The agent NEVER listens again without a fresh wake word.
        """
        end = time.time() + max_wait
        t0 = time.time()
        quiet_start: float | None = None
        while time.time() < end and not self._stop:
            self.ring.pop()  # discard echo / any audio
            if time.time() - t0 < cooldown:
                # hard cooldown: not listening at all yet
                time.sleep(0.02)
                continue
            if self.level < quiet_thresh:
                if quiet_start is None:
                    quiet_start = time.time()
                elif time.time() - quiet_start >= quiet_s:
                    return
            else:
                quiet_start = None
            time.sleep(0.02)

    def run(self) -> None:
        if self.state_path:
            self._heartbeat = threading.Thread(target=self._heartbeat_loop, daemon=True)
            self._heartbeat.start()
        print(f"[voice] ready — say '{self.args.wake_word or 'alexa'}' (default wake model set). "
              f"Ctrl-C to exit.", file=sys.stderr)
        while not self._stop:
            try:
                clock = Clock()
                self.wait_wake(clock)
                if self._stop:
                    break
                audio = self.capture_utterance(clock)
                if len(audio) < SAMPLE_RATE * 0.2:
                    print("[voice] utterance too short — ignoring", file=sys.stderr)
                    continue
                text = self.transcribe(audio, clock)
                # Utterance gate: don't act on near-silence. A false wake fires
                # on ambient, captures ~12s of quiet, and whisper hallucinates
                # a word ("you") — sending that to the harness made it speak
                # on its own. Require real speech in the audio.
                if not text or not self.vad.has_speech(audio):
                    print("[voice] no speech recognized", file=sys.stderr)
                    continue
                print(f"[voice] you: {text}", file=sys.stderr)
                answer = self.ask_harness(text, clock)
                if not answer:
                    print("[voice] empty response from harness", file=sys.stderr)
                    continue
                self.response = answer
                self.write_state()
                print(f"[voice] agent: {answer}", file=sys.stderr)
                self.stage = "tts"
                self.write_state()
                self.speak(answer, clock)
                # Re-arm wake ONLY after our echo has settled out of the mic.
                # Until then, wake detection is not consulted, so our own voice
                # can never start a transcription.
                self._settle_after_speak()
                self.stage = "idle"
                self.write_state()
                clock.report()
            except KeyboardInterrupt:
                break
            except Exception as e:  # keep the loop alive; surface and continue
                print(f"[voice] ERROR: {type(e).__name__}: {e}", file=sys.stderr)
                clock.report()


def main() -> None:
    ap = argparse.ArgumentParser(description="Wake→STT→harness→TTS voice loop (Cortex MK3)")
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--bin", type=Path, default=DEFAULT_BIN)
    ap.add_argument("--whisper", default="base.en", help="whisper size (tiny.en/base.en/small.en)")
    ap.add_argument("--voice", default="af_heart", help="kokoro voice (af_heart, af_alloy, …)")
    ap.add_argument("--tts-device", default="cuda", help="kokoro device (cuda|cpu)")
    ap.add_argument("--stop-silence", type=float, default=0.5, help="VAD silence to end utterance (s)")
    ap.add_argument("--max-utterance", type=float, default=12.0)
    ap.add_argument("--llm-timeout", type=float, default=90.0)
    ap.add_argument("--device", default=None, help="input device index/name (sounddevice)")
    ap.add_argument("--out-device", default=None, help="output device index/name")
    ap.add_argument("--provider", default=None, help="LLM provider override")
    ap.add_argument("--wake-word", default="morpheus", help="wake word (Vosk keyword; must be in the model's vocabulary)")
    ap.add_argument("--vosk-model", default=os.path.join(os.path.dirname(__file__), "wake-model", "vosk-small-en"), help="Vosk model dir for word-level wake detection")
    ap.add_argument("--mic-gain", type=float, default=4.5, help="software gain on mic input (clipping-free; mic is quiet)")
    ap.add_argument("--debug-wake", action="store_true", help="log live Vosk partial + mic peak while waiting for wake")
    ap.add_argument("--state-out", type=Path, default=None,
                    help="write machine-readable pipeline state JSON here (console UI)")
    args = ap.parse_args()

    loop = VoiceLoop(args)
    loop.start_mic()
    try:
        loop.run()
    finally:
        loop.stop_mic()
        print("[voice] bye", file=sys.stderr)


if __name__ == "__main__":
    main()
