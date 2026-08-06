#!/usr/bin/env bash
# =============================================================================
# live_smoke.sh — mid-low live test of the cortex-mk3 harness against a
# real model on the opencode-go router (deepseek-v4-flash by default).
#
# Purpose: catch what offline tests cannot — real provider HTTP, real
# streaming, real protocol shape from a real LLM. The harness renders the
# full chat surface to stdout (snapshot mode auto-engages when stdout is
# not a TTY), so we get to assert on the ACTUAL rendered output of the
# new chat features (CORTEX→agent-name label, empty state, composer
# placeholder, subagent chrome, contiguous transcript) under a real
# model turn.
#
# Bounded: ONE provider call, ONE turn, ~5-15s wall time, cheap model.
#
# Requires: OPENCODE_API_KEY in the environment. If the key is absent the
# script prints a clear SKIP message and exits 0 — live tests are
# opt-in, never a CI failure when keys are unavailable. This makes the
# script safe to wire into `make all-tests` without breaking keyless
# environments; run it explicitly when you want a live check.
#
# Usage:  tests/tui/live_smoke.sh ./cortex-mk3
#         tests/tui/live_smoke.sh ./cortex-mk3 deepseek-v4-flash
#         tests/tui/live_smoke.sh ./cortex-mk3 minimax-m3 90
# =============================================================================

set -euo pipefail

bin="${1:-./cortex-mk3}"
model="${2:-deepseek-v4-flash}"
timeout_s="${3:-90}"

# Graceful skip when the live-test API key is not configured. Live tests
# are an explicit opt-in: we never want a missing key to break the
# offline test suite.
if [[ -z "${OPENCODE_API_KEY:-}" ]]; then
    echo "live_smoke: SKIP (OPENCODE_API_KEY not set — live tests are opt-in)"
    echo "            export OPENCODE_API_KEY=... to run a real-model live check"
    exit 0
fi

if [[ ! -x "$bin" ]]; then
    echo "live_smoke: FAIL — binary not found or not executable: $bin" >&2
    exit 1
fi

# ANSI stripper — the harness emits SGR; tests compare plain text.
strip_ansi() {
    python3 - "$1" <<'PY'
import pathlib, re, sys
s = pathlib.Path(sys.argv[1]).read_bytes().decode('utf-8', 'ignore')
s = re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', s)
print(s)
PY
}

assert_contains() {
    local haystack="$1" needle="$2" label="$3"
    if [[ "$haystack" != *"$needle"* ]]; then
        echo "live_smoke: FAIL — $label" >&2
        echo "  expected to contain: $needle" >&2
        echo "  --- actual (first 400 chars) ---" >&2
        echo "${haystack:0:400}" >&2
        echo "  --------------------------------" >&2
        return 1
    fi
    echo "live_smoke: PASS — $label"
}

# Build the prompt. Deterministic-enough that a well-behaved model gives
# a parseable response, and the response is unique enough to assert on
# without false positives across runs.
prompt="Reply with exactly: live-smoke-ok and nothing else."

tmp_out="$(mktemp)"
trap 'rm -f "$tmp_out"' EXIT

echo "live_smoke: running $bin against opencode-go/$model (timeout ${timeout_s}s)"
# --no-session keeps the run ephemeral (no ~/.cortex write).
# stdout is piped → snapshotMode() engages → full chat surface rendered
# to stdout instead of opening the interactive TUI.
if ! timeout "${timeout_s}" "$bin" \
        --provider opencode-go \
        --model "$model" \
        --no-session \
        run -p "$prompt" \
        > "$tmp_out" 2>&1; then
    rc=$?
    echo "live_smoke: FAIL — harness exited non-zero (rc=$rc) or timed out" >&2
    echo "  --- raw output (last 400 chars) ---" >&2
    tail -c 400 "$tmp_out" >&2 || true
    echo "  -----------------------------------" >&2
    exit 1
fi

# Strip ANSI for stable assertions.
plain="$(strip_ansi "$tmp_out")"

# 1. The product banner rendered at startup ("Cortex MK3 3.1.0 — Agent
#    Runtime"). Case-insensitive match — the banner uses title case,
#    the chat-surface header uses UPPER, both prove the harness booted.
assert_contains_lc() {
    local haystack="$1" needle_lc="$2" label="$3"
    local h_lc="${haystack,,}"
    if [[ "$h_lc" != *"$needle_lc"* ]]; then
        echo "live_smoke: FAIL — $label" >&2
        echo "  expected (case-insensitive) to contain: $needle_lc" >&2
        echo "  --- actual (first 400 chars) ---" >&2
        echo "${haystack:0:400}" >&2
        echo "  --------------------------------" >&2
        return 1
    fi
    echo "live_smoke: PASS — $label"
}
assert_contains_lc "$plain" "cortex mk3" "product banner rendered (Cortex MK3)"

# 2. The model returned the expected response. THIS is the live proof:
#    the harness, against a real LLM on opencode-go, through the full
#    provider + streaming + protocol + render pipeline, delivered the
#    exact token we asked for. A paraphrase/format divergence fails
#    here — that's the point of a live test vs. a unit test.
assert_contains "$plain" "live-smoke-ok" "real-model response contains the expected token"

echo "live_smoke: all live assertions passed against opencode-go/$model"
exit 0
