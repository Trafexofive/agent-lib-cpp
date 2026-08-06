#!/usr/bin/env bash
set -euo pipefail

if rg -n '#include[[:space:]]+["<]([^">]*/)?tui/' src/ui; then
  echo "ui architecture violation: src/ui must not depend on src/tui" >&2
  exit 1
fi

if rg -n '#include[[:space:]]+["<](\.\./)+tui/' src/ui; then
  echo "ui architecture violation: relative src/ui -> src/tui include" >&2
  exit 1
fi

echo "ui architecture smoke passed: src/ui is independent of src/tui"
