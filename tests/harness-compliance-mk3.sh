#!/usr/bin/env bash
# harness-compliance-mk3.sh — Test harness compliance through actual cortex-mk3 binary.
# This uses the real runtime with full system prompt wrapping (harness + persona + tools + feeds).
# Usage: ./tests/harness-compliance-mk3.sh [--harness FILE] [--model MODEL] [--repeat N]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
MK3="$REPO_DIR/cortex-mk3"
HARNESS_DIR="$REPO_DIR/config/harness"

# Defaults
HARNESS="$HARNESS_DIR/default.md"
MODELS=("llama-3.3-70b-versatile" "meta-llama/llama-4-scout-17b-16e-instruct" "qwen/qwen3-32b" "openai/gpt-oss-120b")
PROVIDER="groq"
REPEAT=1
PROMPT="What is 2+2? Answer with just the number."
DELAY=10
TIMEOUT=60

# Parse args
while [[ $# -gt 0 ]]; do
  case "$1" in
    --harness) HARNESS="$2"; shift 2 ;;
    --model) MODELS=("$2"); shift 2 ;;
    --repeat) REPEAT="$2"; shift 2 ;;
    --prompt) PROMPT="$2"; shift 2 ;;
    --delay) DELAY="$2"; shift 2 ;;
    --provider) PROVIDER="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

echo "========================================================================"
echo "MK3 Harness Compliance Test — Real Binary"
echo "========================================================================"
echo "Binary:   $MK3"
echo "Harness:  $HARNESS ($(wc -c < "$HARNESS") bytes)"
echo "Provider: $PROVIDER"
echo "Models:   ${MODELS[*]}"
echo "Prompt:   $PROMPT"
echo "Repeat:   $REPEAT"
echo ""

PASS=0
FAIL=0
ERRORS=0
RESULTS=()

for MODEL in "${MODELS[@]}"; do
  for run in $(seq 1 "$REPEAT"); do
    LABEL="[run $run/$REPEAT]"
    
    # Run cortex-mk3 with --raw to get clean output
    OUTPUT=$(timeout "$TIMEOUT" "$MK3" --raw --provider "$PROVIDER" --model "$MODEL" \
      --harness "$HARNESS" run -p "$PROMPT" 2>&1) || {
      # Check if it's a rate limit or other error
      if echo "$OUTPUT" | grep -q "429"; then
        CODE="429"
        ERRORS=$((ERRORS + 1))
        RESULTS+=("$MODEL  $LABEL  ❌ 429 rate limited")
        echo "  $MODEL  $LABEL  ❌ 429 rate limited"
        sleep "$DELAY"
        continue
      elif echo "$OUTPUT" | grep -q "413"; then
        CODE="413"
        ERRORS=$((ERRORS + 1))
        RESULTS+=("$MODEL  $LABEL  ❌ 413 request too large")
        echo "  $MODEL  $LABEL  ❌ 413 request too large"
        continue
      else
        ERRORS=$((ERRORS + 1))
        RESULTS+=("$MODEL  $LABEL  ❌ ERROR: $(echo "$OUTPUT" | tail -1 | head -c 80)")
        echo "  $MODEL  $LABEL  ❌ ERROR: $(echo "$OUTPUT" | tail -1 | head -c 80)"
        sleep "$DELAY"
        continue
      fi
    }

    # Classify output
    if echo "$OUTPUT" | grep -q '<response final="true">'; then
      CODE="Rf✅"
      PASS=$((PASS + 1))
    elif echo "$OUTPUT" | grep -q '<response>'; then
      CODE="R⚠️"
      FAIL=$((FAIL + 1))
    elif echo "$OUTPUT" | grep -q '<action'; then
      CODE="action-only"
      FAIL=$((FAIL + 1))
    elif [ -z "$OUTPUT" ] || [ -z "$(echo "$OUTPUT" | tr -d '[:space:]')" ]; then
      CODE="empty"
      FAIL=$((FAIL + 1))
    else
      CODE="bare"
      FAIL=$((FAIL + 1))
    fi

    # Show preview
    PREVIEW=$(echo "$OUTPUT" | head -1 | head -c 80)
    RESULTS+=("$MODEL  $LABEL  $CODE  $PREVIEW")
    echo "  $MODEL  $LABEL  $CODE  $PREVIEW"

    sleep "$DELAY"
  done
done

echo ""
echo "========================================================================"
echo "SUMMARY"
echo "========================================================================"
echo "Pass: $PASS  Fail: $FAIL  Errors: $ERRORS  Total: $((PASS + FAIL + ERRORS))"
echo ""

# Per-model best result
declare -A BEST
for r in "${RESULTS[@]}"; do
  MODEL=$(echo "$r" | awk '{print $1}')
  CODE=$(echo "$r" | awk '{print $3}')
  if [[ "$CODE" == "Rf✅" ]]; then
    BEST[$MODEL]="Rf✅"
  elif [[ -z "${BEST[$MODEL]+x}" ]] && [[ "$CODE" != "Rf✅" ]]; then
    BEST[$MODEL]="$CODE"
  fi
done

echo "Model                              Best"
echo "----------------------------------------"
for MODEL in "${!BEST[@]}"; do
  printf "%-35s %s\n" "$MODEL" "${BEST[$MODEL]}"
done

# Exit 0 if all pass
if [[ $FAIL -eq 0 && $ERRORS -eq 0 ]]; then
  exit 0
else
  exit 1
fi
