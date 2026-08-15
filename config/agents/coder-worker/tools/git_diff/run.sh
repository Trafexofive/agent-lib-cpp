#!/usr/bin/env bash
# git_diff — bounded read-only diff. Params JSON on stdin and/or $1 file.
set -euo pipefail

read_params() {
  if [[ $# -ge 1 && -n "${1:-}" && -f "$1" ]]; then
    cat -- "$1"
  else
    cat || true
  fi
}

raw=$(read_params "$@")
cwd="."; dstat="true"; staged="false"; path=""; max_bytes=100000
if command -v python3 >/dev/null 2>&1; then
  eval "$(printf '%s' "$raw" | python3 -c '
import json,sys,shlex
try: d=json.loads(sys.stdin.read() or "{}")
except Exception: d={}
print("cwd="+shlex.quote(str(d.get("cwd") or ".")))
print("dstat="+("true" if bool(d.get("stat", True)) else "false"))
print("staged="+("true" if bool(d.get("staged", False)) else "false"))
print("path="+shlex.quote(str(d.get("path") or "")))
print("max_bytes="+str(int(d.get("max_bytes") or 100000)))
' 2>/dev/null)" || true
fi

cd "$cwd" 2>/dev/null || { echo '{"success":false,"error":"cwd not found"}'; exit 0; }
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo '{"success":false,"error":"not a git repository"}'
  exit 0
fi

cmd=(git diff --no-color)
[[ "$staged" == "true" ]] && cmd+=(--cached)
[[ "$dstat" == "true" ]] && cmd+=(--stat)
[[ -n "$path" ]] && cmd+=(-- "$path")
out=$("${cmd[@]}" 2>&1 || true)

MAXB="$max_bytes" python3 -c '
import json,os,sys
out=sys.stdin.read()
maxb=int(os.environ.get("MAXB","100000"))
raw=out.encode("utf-8","replace")
trunc=len(raw)>maxb
if trunc: out=raw[:maxb].decode("utf-8","replace")+"\n…[truncated]"
print(json.dumps({"success":True,"truncated":trunc,"bytes":len(out),"output":out},ensure_ascii=False))
' <<<"$out"
