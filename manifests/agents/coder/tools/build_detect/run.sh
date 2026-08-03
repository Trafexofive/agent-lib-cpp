#!/usr/bin/env bash
# build_detect — scan repo for build/test surfaces. Params on stdin and/or $1.
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
if command -v python3 >/dev/null 2>&1; then
  cwd=$(printf '%s' "$raw" | python3 -c '
import json,sys
try: d=json.loads(sys.stdin.read() or "{}")
except Exception: d={}
print(d.get("cwd") or ".")
' 2>/dev/null) || cwd="."
fi

cd "$cwd" 2>/dev/null || { echo '{"success":false,"error":"cwd not found"}'; exit 0; }

python3 - <<'PY'
import json
from pathlib import Path
root = Path(".").resolve()
hits = []
checks = [
  ("Makefile", "make"),
  ("CMakeLists.txt", "cmake"),
  ("package.json", "npm/yarn/pnpm"),
  ("Cargo.toml", "cargo"),
  ("go.mod", "go"),
  ("pyproject.toml", "python"),
  ("setup.py", "python"),
  ("meson.build", "meson"),
  ("build.gradle", "gradle"),
  ("pom.xml", "maven"),
  ("Justfile", "just"),
  ("justfile", "just"),
]
for name, kind in checks:
  if (root / name).exists():
    hits.append({"file": name, "system": kind})

suggest = []
if (root / "Makefile").exists():
  suggest.extend(["make test", "make check", "make cortex-mk3"])
if (root / "package.json").exists():
  suggest.append("npm test")
if (root / "Cargo.toml").exists():
  suggest.append("cargo test")
if (root / "go.mod").exists():
  suggest.append("go test ./...")
if (root / "CMakeLists.txt").exists():
  suggest.append("ctest --output-on-failure")

print(json.dumps({
  "success": True,
  "hits": hits,
  "suggest_verify": suggest,
  "cwd": str(root),
}, ensure_ascii=False))
PY
