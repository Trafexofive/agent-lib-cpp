#!/usr/bin/env bash
# One command: start the voice backend (single instance) and launch the
# Python/Textual TUI. Quitting the TUI stops the backend.
#
#   ./run.sh                 # clean interview mic
#   VOICE_DEVICE=NN ./run.sh # override mic
set -euo pipefail
cd "$(dirname "$0")"

ROOT="../.."
# ONLY the clean interview mic. Pin by exact name so index shifts and virtual
# sources can never hijack the input. Override with VOICE_DEVICE=<index|name>.
VDEVICE="${VOICE_DEVICE:-USB Composite Device Mono}"
# Wake word — swap anytime. With local-wake it's a RECORDED phrase: record
# wake-refs/<phrase>-*.wav via record_wake.py, then set VOICE_WAKE to match.
VWAKE="${VOICE_WAKE:-morpheus}"
# Set VOICE_DEBUG_WAKE=1 to log live Vosk partials while waiting for wake.
DBGWAKE=""
if [[ "${VOICE_DEBUG_WAKE:-0}" == "1" ]]; then DBGWAKE="--debug-wake"; fi
VOICE_STATE="${VOICE_STATE:-/tmp/voice_console_state.json}"
VOICE_LOG="/tmp/voice_console.log"
PY="$ROOT/playground/local-transcription/.venv/bin/python"
SCRIPT="$ROOT/playground/local-transcription/voice.py"

# ── single-instance: kill this project's backend deterministically via its
#    PID file (or pkill fallback), then start one fresh. ──
stop_backend() {
    local pid="$VOICE_STATE.pid"
    if [[ -f "$pid" ]]; then
        kill "$(cat "$pid")" 2>/dev/null || true
        rm -f "$pid"
    fi
    pkill -f "$SCRIPT" 2>/dev/null || true
}
stop_backend
sleep 0.5
echo "[voice-console] starting voice backend (log: $VOICE_LOG)"
if [[ -n "$VDEVICE" ]]; then
    nohup "$PY" "$SCRIPT" --device "$VDEVICE" --wake-word "$VWAKE" \
        --vosk-model "$ROOT/playground/local-transcription/wake-model/vosk-small-en" \
        $DBGWAKE --state-out "$VOICE_STATE" \
        >"$VOICE_LOG" 2>&1 &
else
    nohup "$PY" "$SCRIPT" --wake-word "$VWAKE" \
        --vosk-model "$ROOT/playground/local-transcription/wake-model/vosk-small-en" \
        --state-out "$VOICE_STATE" \
        >"$VOICE_LOG" 2>&1 &
fi

export CORTEX_BIN="$ROOT/cortex-mk3"
export VOICE_MANIFEST="$ROOT/playground/local-transcription/manifests/agents/voice/agent.yml"
export VOICE_STATE="$VOICE_STATE"

echo "[voice-console] launching TUI (Textual) — q quits. Say \"$VWAKE\" to wake."
# Run the Textual TUI; when it exits, stop the voice backend so nothing lingers.
"$PY" "$ROOT/playground/local-transcription/voice_console.py"
rc=$?
stop_backend
exit $rc
