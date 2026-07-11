#!/usr/bin/env bash
set -euo pipefail

bin="${1:-./cortex-mk3}"
if [[ ! -x "$bin" ]]; then
  echo "missing executable: $bin" >&2
  exit 2
fi

tmp="${TMPDIR:-/tmp}/mk3-repl-parity.$$"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT

run_one() {
  local mode="$1"
  (printf '/quit\r'; sleep 0.1) | \
    timeout 5 env TERM=xterm COLUMNS=100 LINES=28 \
    "$bin" --tui "$mode" --provider openai-codex --model gpt-5.5 --no-session \
    >"$tmp/$mode.ansi" 2>"$tmp/$mode.err"
}

run_one legacy
run_one inkcell

cmp -s "$tmp/legacy.ansi" "$tmp/inkcell.ansi" || {
  echo "ANSI output differs between --tui legacy and --tui inkcell" >&2
  diff -u "$tmp/legacy.ansi" "$tmp/inkcell.ansi" | head -200 >&2 || true
  exit 1
}

cmp -s "$tmp/legacy.err" "$tmp/inkcell.err" || {
  echo "stderr differs between --tui legacy and --tui inkcell" >&2
  diff -u "$tmp/legacy.err" "$tmp/inkcell.err" | head -200 >&2 || true
  exit 1
}

echo "repl parity smoke passed: legacy == inkcell"
