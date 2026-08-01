#!/usr/bin/env bash
set -euo pipefail
raw=$(cat || true)
cwd="."; dstat="true"; staged="false"; path=""; max_bytes=100000
if command -v python3 >/dev/null 2>&1; then
  parsed=$(printf '%s' "$raw" | python3 -c '
import json,sys
d=json.loads(sys.stdin.read() or "{}")
print(json.dumps({
  "cwd": d.get("cwd") or ".",
  "stat": bool(d.get("stat", True)),
  "staged": bool(d.get("staged", False)),
  "path": d.get("path") or "",
  "max_bytes": int(d.get("max_bytes") or 100000),
}))
' 2>/dev/null) || parsed="{}"
  cwd=$(printf '%s' "$parsed" | python3 -c 'import json,sys;print(json.load(sys.stdin)["cwd"])')
  dstat=$(printf '%s' "$parsed" | python3 -c 'import json,sys;print("true" if json.load(sys.stdin)["stat"] else "false")')
  staged=$(printf '%s' "$parsed" | python3 -c 'import json,sys;print("true" if json.load(sys.stdin)["staged"] else "false")')
  path=$(printf '%s' "$parsed" | python3 -c 'import json,sys;print(json.load(sys.stdin)["path"])')
  max_bytes=$(printf '%s' "$parsed" | python3 -c 'import json,sys;print(json.load(sys.stdin)["max_bytes"])')
fi
cd "$cwd" 2>/dev/null || { echo '{"success":false,"error":"cwd not found"}'; exit 0; }
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo '{"success":false,"error":"not a git repository"}'; exit 0
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
trunc=len(out.encode())>maxb
if trunc: out=out.encode()[:maxb].decode("utf-8","replace")+"\n…[truncated]"
print(json.dumps({"success":True,"truncated":trunc,"bytes":len(out),"output":out},ensure_ascii=False))
' <<<"$out"
