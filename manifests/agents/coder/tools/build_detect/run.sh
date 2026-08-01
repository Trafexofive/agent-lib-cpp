#!/usr/bin/env bash
set -euo pipefail
raw=$(cat || true)
cwd="."
if command -v python3 >/dev/null 2>&1; then
  cwd=$(printf '%s' "$raw" | python3 -c 'import json,sys
try: d=json.loads(sys.stdin.read() or "{}")
except: d={}
print(d.get("cwd") or ".")' 2>/dev/null) || cwd="."
fi
cd "$cwd" 2>/dev/null || { echo '{"success":false,"error":"cwd not found"}'; exit 0; }
python3 - <<'PY'
import json, os
from pathlib import Path
root = Path(".")
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
]
for name, kind in checks:
  if (root / name).exists():
    hits.append({"file": name, "system": kind})
# suggest verify
suggest = []
if (root / "Makefile").exists():
  suggest.append("make test")
  suggest.append("make check")
if (root / "package.json").exists():
  suggest.append("npm test")
if (root / "Cargo.toml").exists():
  suggest.append("cargo test")
if (root / "go.mod").exists():
  suggest.append("go test ./...")
print(json.dumps({"success": True, "hits": hits, "suggest_verify": suggest, "cwd": str(root.resolve())}, ensure_ascii=False))
PY
