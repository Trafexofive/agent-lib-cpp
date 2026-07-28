#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "$0")/../../../.." && pwd)"

echo "== root module files =="
test -f config/agents/orchestra/agent.yml
test -f config/agents/orchestra/system.md
test -f config/agents/orchestra/persona.md
test -f config/agents/orchestra/workflows/route.yml
test -f config/agents/orchestra/agents/planner/agent.yml
echo OK

echo "== root: path roster + bare coder, ask_tool only =="
rg -q 'ask_tool' config/agents/orchestra/agent.yml
rg -q '\./agents/planner/agent\.yml' config/agents/orchestra/agent.yml
rg -q '^\s*-\s*coder\s*$' config/agents/orchestra/agent.yml
if rg -n "tools:" -A15 config/agents/orchestra/agent.yml | rg -q 'exec|fs_read|fs_write|^[[:space:]]*- grep|^[[:space:]]*- list'; then
  echo "FAIL root has worker tools" >&2
  exit 1
fi
echo OK

echo "== dry-run root + variants + specialists =="
for m in \
  config/agents/orchestra/agent.yml \
  config/agents/orchestra/agents/planner/agent.yml \
  config/agents/orchestra/agents/researcher/agent.yml \
  config/agents/orchestra/agents/skeptic/agent.yml \
  config/agents/orchestra/helmsman/agent.yml \
  config/agents/orchestra/forge/agent.yml \
  config/agents/orchestra/lens/agent.yml \
  config/agents/orchestra/archon/agent.yml
do
  echo "-- $m"
  ./cortex-mk3 --dry-run -m "$m" -p "noop" --raw >/dev/null
done
echo PASS
