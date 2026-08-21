#!/usr/bin/env bash
# live-lab graph check. LLM only when CORTEX_LIVE=1.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
MANIFEST="$ROOT/manifests/agents/live-lab/agent.yml"
BIN="${CORTEX_BIN:-$HOME/.local/bin/cortex-mk3}"
if [[ ! -x "$BIN" ]]; then
  BIN="$ROOT/cortex-mk3"
fi
if [[ ! -x "$BIN" ]]; then
  echo "no cortex-mk3 binary (install or set CORTEX_BIN)" >&2
  exit 1
fi

echo "== list --agents (must include live-lab) =="
"$BIN" list --agents 2>/dev/null | grep -E 'live-lab|echo-worker|probe-worker' || {
  echo "WARN: list --agents did not print live-lab (still trying dry-run)" >&2
}

echo "== dry-run load =="
"$BIN" --dry-run -m "$MANIFEST" 2>&1 | tail -20

if [[ "${CORTEX_LIVE:-0}" != "1" ]]; then
  echo "== skip LLM (set CORTEX_LIVE=1 to ping) =="
  exit 0
fi

echo "== live ephemeral present-yourself =="
"$BIN" --headless --ephemeral -m "$MANIFEST" -p "present yourself in one short final. name, engine, children. no tools." 2>&1 | tail -40
