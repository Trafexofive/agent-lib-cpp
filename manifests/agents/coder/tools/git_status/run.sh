#!/usr/bin/env bash
set -euo pipefail
raw=$(cat || true)
cwd="."
short="true"
if command -v python3 >/dev/null 2>&1; then
  parsed=$(printf '%s' "$raw" | python3 -c '
import json,sys
try: d=json.loads(sys.stdin.read() or "{}")
except Exception: d={}
print(json.dumps({"cwd": d.get("cwd") or ".", "short": bool(d.get("short", True))}))
' 2>/dev/null) || parsed='{"cwd":".","short":true}'
  cwd=$(printf '%s' "$parsed" | python3 -c 'import json,sys; print(json.load(sys.stdin)["cwd"])')
  short=$(printf '%s' "$parsed" | python3 -c 'import json,sys; print("true" if json.load(sys.stdin)["short"] else "false")')
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
