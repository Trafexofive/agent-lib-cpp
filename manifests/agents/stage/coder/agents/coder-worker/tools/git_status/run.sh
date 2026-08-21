#!/usr/bin/env bash
# git_status — read-only status snapshot. Params JSON on stdin and/or $1 file.
set -euo pipefail

read_params() {
  if [[ $# -ge 1 && -n "${1:-}" && -f "$1" ]]; then
    cat -- "$1"
  else
    cat || true
  fi
}

raw=$(read_params "$@")
cwd="."
short="true"
if command -v python3 >/dev/null 2>&1; then
  eval "$(printf '%s' "$raw" | python3 -c '
import json,sys,shlex
try: d=json.loads(sys.stdin.read() or "{}")
except Exception: d={}
cwd=d.get("cwd") or "."
short=bool(d.get("short", True))
print("cwd="+shlex.quote(str(cwd)))
print("short="+("true" if short else "false"))
' 2>/dev/null)" || true
fi

cd "$cwd" 2>/dev/null || { echo '{"success":false,"error":"cwd not found"}'; exit 0; }

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo '{"success":true,"git_repo":false,"output":"not a git repository"}'
  exit 0
fi

branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
commit=$(git rev-parse --short HEAD 2>/dev/null || echo "?")
if [[ "$short" == "true" ]]; then
  st=$(git status --porcelain=v1 -b 2>/dev/null || true)
else
  st=$(git status 2>/dev/null || true)
fi

export B="$branch" C="$commit" S="$st"
python3 -c 'import json,os; print(json.dumps({"success":True,"git_repo":True,"branch":os.environ["B"],"commit":os.environ["C"],"output":os.environ["S"]},ensure_ascii=False))'
