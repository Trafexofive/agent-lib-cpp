#!/usr/bin/env bash
# project_test — run explicit or auto-detected verify command.
# Params JSON on stdin and/or $1 file.
set -euo pipefail

read_params() {
  if [[ $# -ge 1 && -n "${1:-}" && -f "$1" ]]; then
    cat -- "$1"
  else
    cat || true
  fi
}

raw=$(read_params "$@")
cwd="."; command=""; timeout_sec=120
if command -v python3 >/dev/null 2>&1; then
  eval "$(printf '%s' "$raw" | python3 -c '
import json,sys,shlex
try: d=json.loads(sys.stdin.read() or "{}")
except Exception: d={}
print("cwd="+shlex.quote(str(d.get("cwd") or ".")))
print("command="+shlex.quote(str(d.get("command") or "")))
print("timeout_sec="+str(int(d.get("timeout_sec") or 120)))
' 2>/dev/null)" || true
fi

cd "$cwd" 2>/dev/null || { echo '{"success":false,"error":"cwd not found"}'; exit 0; }

emit() {
  local ec="$1" cmd="$2" out="$3" auto="${4:-false}"
  EC="$ec" CMD="$cmd" AUTO="$auto" python3 -c '
import json,sys,os
out=sys.stdin.read()
maxb=200000
trunc=len(out)>maxb
if trunc: out=out[:maxb]+"\n…[truncated]"
print(json.dumps({
  "success": int(os.environ["EC"])==0,
  "exit_code": int(os.environ["EC"]),
  "command": os.environ["CMD"],
  "auto": os.environ.get("AUTO","false")=="true",
  "truncated": trunc,
  "output": out,
}, ensure_ascii=False))
' <<<"$out"
}

run_one() {
  local cmd="$1" auto="${2:-false}"
  local out ec
  set +e
  out=$(timeout "$timeout_sec" bash -lc "$cmd" 2>&1)
  ec=$?
  set -e
  emit "$ec" "$cmd" "$out" "$auto"
}

if [[ -n "$command" ]]; then
  run_one "$command" false
  exit 0
fi

# Auto-detect: first matching surface wins (narrowest common defaults).
for candidate in \
  "make test" \
  "make check" \
  "ctest --output-on-failure" \
  "npm test" \
  "cargo test" \
  "pytest -q" \
  "go test ./..."
do
  case "$candidate" in
    make*) [[ -f Makefile ]] || continue ;;
    ctest*) [[ -d build || -f CMakeCache.txt || -f CMakeLists.txt ]] || continue ;;
    npm*) [[ -f package.json ]] || continue ;;
    cargo*) [[ -f Cargo.toml ]] || continue ;;
    pytest*) [[ -f pytest.ini || -f pyproject.toml || -d tests ]] || continue ;;
    go*) [[ -f go.mod ]] || continue ;;
  esac
  run_one "$candidate" true
  exit 0
done

echo '{"success":false,"error":"no verify command detected; pass params.command"}'
