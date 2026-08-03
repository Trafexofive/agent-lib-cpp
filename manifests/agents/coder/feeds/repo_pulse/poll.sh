#!/usr/bin/env bash
# repo_pulse feed — ambient git pulse for the coding worktree.
set -euo pipefail

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo '{"git_repo":false,"dirty":false,"branch":"","commit":"","porcelain":""}'
  exit 0
fi

branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
commit=$(git rev-parse --short HEAD 2>/dev/null || echo "?")
porcelain=$(git status --porcelain=v1 2>/dev/null | head -n 40 || true)
dirty=false
[[ -n "$porcelain" ]] && dirty=true

export B="$branch" C="$commit" D="$dirty" P="$porcelain"
python3 -c 'import json,os; print(json.dumps({"git_repo":True,"branch":os.environ["B"],"commit":os.environ["C"],"dirty":os.environ["D"]=="true","porcelain":os.environ["P"]},ensure_ascii=False))'
