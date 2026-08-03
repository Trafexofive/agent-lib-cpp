#!/usr/bin/env bash
set -euo pipefail
raw=$(cat || true)
cwd="."; command=""; timeout_sec=120
if command -v python3 >/dev/null 2>&1; then
  parsed=$(printf '%s' "$raw" | python3 -c '
import json,sys
d=json.loads(sys.stdin.read() or "{}")
print(json.dumps({"cwd":d.get("cwd") or ".","command":d.get("command") or "","timeout_sec":int(d.get("timeout_sec") or 120)}))
' 2>/dev/null) || parsed='{"cwd":".","command":"","timeout_sec":120}'
  cwd=$(printf '%s' "$parsed" | python3 -c 'import json,sys;print(json.load(sys.stdin)["cwd"])')
  command=$(printf '%s' "$parsed" | python3 -c 'import json,sys;print(json.load(sys.stdin)["command"])')
  timeout_sec=$(printf '%s' "$parsed" | python3 -c 'import json,sys;print(json.load(sys.stdin)["timeout_sec"])')
fi
cd "$cwd" 2>/dev/null || { echo '{"success":false,"error":"cwd not found"}'; exit 0; }
export timeout_sec
if [[ -n "$command" ]]; then
  EC=0; CMD="$command"
  set +e
  out=$(timeout "$timeout_sec" bash -lc "$command" 2>&1)
  ec=$?
  set -e
  EC=$ec CMD=$command python3 -c '
import json,sys,os
out=sys.stdin.read()
maxb=200000
trunc=len(out)>maxb
if trunc: out=out[:maxb]+"\n…[truncated]"
print(json.dumps({"success": int(os.environ["EC"])==0,"exit_code":int(os.environ["EC"]),"command":os.environ["CMD"],"truncated":trunc,"output":out},ensure_ascii=False))
' <<<"$out"
  exit 0
fi
# auto-detect order
for candidate in "make test" "make check" "ctest --output-on-failure" "npm test" "cargo test" "pytest -q" "go test ./..."; do
  case "$candidate" in
    make*) [[ -f Makefile ]] || continue ;;
    ctest*) [[ -d build || -f CMakeCache.txt || -f CMakeLists.txt ]] || continue ;;
    npm*) [[ -f package.json ]] || continue ;;
    cargo*) [[ -f Cargo.toml ]] || continue ;;
    pytest*) [[ -f pytest.ini || -f pyproject.toml || -d tests ]] || continue ;;
    go*) [[ -f go.mod ]] || continue ;;
  esac
  set +e
  out=$(timeout "$timeout_sec" bash -lc "$candidate" 2>&1)
  ec=$?
  set -e
  EC=$ec CMD=$candidate python3 -c '
import json,sys,os
out=sys.stdin.read()
maxb=200000
trunc=len(out)>maxb
if trunc: out=out[:maxb]+"\n…[truncated]"
print(json.dumps({"success": int(os.environ["EC"])==0,"exit_code":int(os.environ["EC"]),"command":os.environ["CMD"],"auto":True,"truncated":trunc,"output":out},ensure_ascii=False))
' <<<"$out"
  exit 0
done
echo '{"success":false,"error":"no verify command detected; pass params.command"}'
