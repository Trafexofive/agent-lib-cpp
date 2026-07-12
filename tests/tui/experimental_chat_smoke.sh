#!/usr/bin/env bash
set -euo pipefail

bin="${1:-./cortex-mk3}"

strip_ansi() {
  python3 - "$1" <<'PY'
import pathlib, re, sys
s = pathlib.Path(sys.argv[1]).read_bytes().decode('utf-8', 'ignore')
s = re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', s)
print(s)
PY
}

assert_contains() {
  local haystack="$1" needle="$2"
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "missing expected text: $needle" >&2
    return 1
  fi
}

assert_not_contains() {
  local haystack="$1" needle="$2"
  if [[ "$haystack" == *"$needle"* ]]; then
    echo "unexpected text present: $needle" >&2
    return 1
  fi
}

empty_out="$(mktemp)"
empty_err="$(mktemp)"
coder_out="$(mktemp)"
coder_err="$(mktemp)"
trap 'rm -f "$empty_out" "$empty_err" "$coder_out" "$coder_err"' EXIT

timeout 8 env MK3_TUI_SNAPSHOT=1 "$bin" --tui experimental --no-session >"$empty_out" 2>"$empty_err"
timeout 8 env MK3_TUI_SNAPSHOT=1 "$bin" -m manifests/agents/coder/agent.yml --tui experimental --no-session >"$coder_out" 2>"$coder_err"

empty="$(strip_ansi "$empty_out")"
coder="$(strip_ansi "$coder_out")"

assert_contains "$empty" "CORTEX MK3"
assert_contains "$empty" "› █"
assert_contains "$coder" "CORTEX MK3"
assert_contains "$coder" "coder"
assert_contains "$coder" "› █"

for removed in "No turns yet." "Type a prompt and press Enter to send." "Esc focuses history"; do
  assert_not_contains "$empty" "$removed"
  assert_not_contains "$coder" "$removed"
done

for bad in "control board" "Chat / Agent History" "Harness / Manifest" "╭─ composer" "composer (focus)" "╰"; do
  assert_not_contains "$empty" "$bad"
  assert_not_contains "$coder" "$bad"
done

echo "experimental chat smoke passed: chat-only surface, no main/menu/boxed composer"
