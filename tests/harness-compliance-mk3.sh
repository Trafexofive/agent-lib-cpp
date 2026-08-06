#!/usr/bin/env bash
# harness-compliance-mk3.sh — Test harness compliance through actual cortex-mk3 binary.
# Uses the real runtime (harness + persona + tools). Scores protocol from raw.md
# written under a temp workdir — never from TUI stdout (experimental TUI is default).
#
# Usage:
#   ./tests/harness-compliance-mk3.sh [--harness FILE] [--model MODEL] [--repeat N]
#   ./tests/harness-compliance-mk3.sh --provider opencode --model deepseek-v4-flash-free --repeat 2
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
MK3="$REPO_DIR/cortex-mk3"
HARNESS_DIR="$REPO_DIR/manifests/harness"

# Defaults
HARNESS="$HARNESS_DIR/default.md"
MODELS=("deepseek-v4-flash-free")
PROVIDER="opencode"
REPEAT=1
PROMPT="What is 2+2? Answer with just the number."
DELAY=2
TIMEOUT=90
MANIFEST="$REPO_DIR/manifests/agents/default/agent.yml"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --harness) HARNESS="$2"; shift 2 ;;
    --model) MODELS=("$2"); shift 2 ;;
    --repeat) REPEAT="$2"; shift 2 ;;
    --prompt) PROMPT="$2"; shift 2 ;;
    --delay) DELAY="$2"; shift 2 ;;
    --provider) PROVIDER="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --manifest) MANIFEST="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

if [[ ! -x "$MK3" ]]; then
  echo "ERROR: binary missing — run: make cortex-mk3" >&2
  exit 2
fi
if [[ ! -f "$HARNESS" ]]; then
  echo "ERROR: harness not found: $HARNESS" >&2
  exit 2
fi

echo "========================================================================"
echo "MK3 Harness Compliance Test — Real Binary"
echo "========================================================================"
echo "Binary:   $MK3"
echo "Manifest: $MANIFEST"
echo "Harness:  $HARNESS ($(wc -c < "$HARNESS") bytes)"
echo "Provider: $PROVIDER"
echo "Models:   ${MODELS[*]}"
echo "Prompt:   $PROMPT"
echo "Repeat:   $REPEAT"
echo "Score:    raw.md under temp workdir (not TUI stdout)"
echo ""

PASS=0
FAIL=0
ERRORS=0
RESULTS=()

classify() {
  local text="$1"
  if echo "$text" | grep -q '<response final="true">'; then
    echo "Rf✅"
  elif echo "$text" | grep -qE '<response[^>]*>'; then
    echo "R⚠️"
  elif echo "$text" | grep -q '<action'; then
    echo "action-only"
  elif [[ -z "${text//[[:space:]]/}" ]]; then
    echo "empty"
  else
    echo "bare"
  fi
}

for MODEL in "${MODELS[@]}"; do
  for run in $(seq 1 "$REPEAT"); do
    LABEL="[run $run/$REPEAT]"
    WORK=$(mktemp -d /tmp/mk3-compliance-XXXXXX)
    # Headless: inkcell one-shot in snapshot mode renders to stdout without
    # owning the terminal (the experimental App owns the interactive -p path).
    # --raw suppresses provider-setup logging so stdout carries only the render.
    (
      cd "$WORK"
      timeout "$TIMEOUT" env MK3_TUI_SNAPSHOT=1 \
        "$MK3" --raw --ephemeral --no-session \
        --provider "$PROVIDER" --model "$MODEL" \
        --harness "$HARNESS" \
        -m "$MANIFEST" \
        run -p "$PROMPT" \
        >"$WORK/stdout.txt" 2>"$WORK/stderr.txt"
    ) || true

    # Prefer raw.md (model wire output). Fallbacks: stdout final result, iterations.
    RAW=""
    if [[ -f "$WORK/raw.md" ]]; then
      RAW=$(cat "$WORK/raw.md")
    elif ls "$WORK"/dev/ephemeral-*/raw.md >/dev/null 2>&1; then
      RAW=$(cat "$WORK"/dev/ephemeral-*/raw.md | head -c 200000)
    elif [[ -s "$WORK/stdout.txt" ]]; then
      RAW=$(cat "$WORK/stdout.txt")
    elif [[ -f "$WORK/iterations.md" ]]; then
      RAW=$(awk '/MODEL\/RUNTIME OUTPUT/{flag=1;next}/^```$/{if(flag){exit}}flag' "$WORK/iterations.md" 2>/dev/null || true)
      [[ -z "$RAW" ]] && RAW=$(cat "$WORK/iterations.md")
    fi

    # Transport errors from stderr
    if grep -qE '429|rate.?limit' "$WORK/stderr.txt" 2>/dev/null; then
      CODE="429"
      ERRORS=$((ERRORS + 1))
      RESULTS+=("$MODEL  $LABEL  $CODE")
      echo "  $MODEL  $LABEL  ❌ 429 rate limited  (workdir $WORK)"
      sleep "$DELAY"
      continue
    fi
    if grep -qE '413|request too large' "$WORK/stderr.txt" 2>/dev/null; then
      CODE="413"
      ERRORS=$((ERRORS + 1))
      RESULTS+=("$MODEL  $LABEL  $CODE")
      echo "  $MODEL  $LABEL  ❌ 413 request too large  (workdir $WORK)"
      continue
    fi
    if [[ -z "${RAW//[[:space:]]/}" ]] && grep -qiE 'error|fail|exception' "$WORK/stderr.txt" 2>/dev/null; then
      CODE="ERROR"
      ERRORS=$((ERRORS + 1))
      PREVIEW=$(tail -1 "$WORK/stderr.txt" | head -c 100)
      RESULTS+=("$MODEL  $LABEL  $CODE  $PREVIEW")
      echo "  $MODEL  $LABEL  ❌ ERROR: $PREVIEW"
      sleep "$DELAY"
      continue
    fi

    CODE=$(classify "$RAW")
    if [[ "$CODE" == "Rf✅" ]]; then
      PASS=$((PASS + 1))
    else
      FAIL=$((FAIL + 1))
    fi
    PREVIEW=$(echo "$RAW" | tr '\n' ' ' | head -c 100)
    RESULTS+=("$MODEL  $LABEL  $CODE  $PREVIEW")
    echo "  $MODEL  $LABEL  $CODE  $PREVIEW"
    # Keep last workdir path for debug
    echo "    workdir: $WORK" >&2

    sleep "$DELAY"
  done
done

echo ""
echo "========================================================================"
echo "SUMMARY"
echo "========================================================================"
echo "Pass: $PASS  Fail: $FAIL  Errors: $ERRORS  Total: $((PASS + FAIL + ERRORS))"
echo ""

echo "Model                              Best"
echo "----------------------------------------"
# shellcheck disable=SC2207
for MODEL in "${MODELS[@]}"; do
  BEST="none"
  for r in "${RESULTS[@]}"; do
    m=$(echo "$r" | awk '{print $1}')
    c=$(echo "$r" | awk '{print $3}')
    [[ "$m" != "$MODEL" ]] && continue
    if [[ "$c" == "Rf✅" ]]; then BEST="Rf✅"; break; fi
    BEST="$c"
  done
  printf "%-35s %s\n" "$MODEL" "$BEST"
done

if [[ $FAIL -eq 0 && $ERRORS -eq 0 ]]; then
  exit 0
else
  exit 1
fi
