#!/usr/bin/env bash
# harness-prompt-measure.sh — measure MK3 harness protocol prompt quality
# TDD baseline: run before edits, run after, compare.
set -euo pipefail

PROMPT="${1:-config/harness/default.md}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

if [ ! -f "$PROMPT" ]; then
  echo "ERROR: $PROMPT not found"
  exit 1
fi

echo "=== Harness Prompt Metrics ==="
echo "file: $PROMPT"
echo "timestamp: $(date -Iseconds)"
echo ""

# ── 1. Raw stats ──────────────────────────────────────────
LINES=$(wc -l < "$PROMPT")
CHARS=$(wc -c < "$PROMPT")
BYTES=$(wc -c < "$PROMPT")
WORDS=$(wc -w < "$PROMPT")

# Approximate token count (words * 1.3 for code/punctuation)
TOKENS_EST=$(python3 -c "print(int($WORDS * 1.3))")

printf "%-30s %s\n" "raw.lines" "$LINES"
printf "%-30s %s\n" "raw.chars" "$CHARS"
printf "%-30s %s\n" "raw.words" "$WORDS"
printf "%-30s %s\n" "raw.tokens_est" "$TOKENS_EST"

# ── 2. Unique word ratio ──────────────────────────────────
# Strip markdown/code fences, lowercase, count unique vs total
UNIQUE_WORDS=$(tr '[:upper:]' '[:lower:]' < "$PROMPT" \
  | sed 's/[^a-z0-9 ]/ /g' \
  | tr -s ' ' '\n' \
  | grep -v '^$' \
  | sort -u | wc -l)
TOTAL_NONUNIQUE=$(tr '[:upper:]' '[:lower:]' < "$PROMPT" \
  | sed 's/[^a-z0-9 ]/ /g' \
  | tr -s ' ' '\n' \
  | grep -v '^$' \
  | wc -l)
UNIQUENESS=$(python3 -c "print(round($UNIQUE_WORDS / $TOTAL_NONUNIQUE * 100, 1))")

printf "%-30s %s\n" "unique.words" "$UNIQUE_WORDS"
printf "%-30s %s\n" "unique.total_words" "$TOTAL_NONUNIQUE"
printf "%-30s %s%%\n" "unique.ratio" "$UNIQUENESS"

# ── 3. Directive/imperative density ────────────────────────
# Count imperative verbs: read, write, use, call, output, think, plan, stop, try, check, run
IMPERATIVES=$(grep -oiE '\b(read|write|use|call|output|think|plan|stop|try|check|run|execute|gather|respond|return|follow|keep|avoid|ensure|verify|test|measure|extract|match|decide|assess)\b' "$PROMPT" | wc -l || echo 0)
DIRECTIVE_DENSITY=$(python3 -c "print(round($IMPERATIVES / $TOKENS_EST * 100, 1))")

printf "%-30s %s\n" "directive.count" "$IMPERATIVES"
printf "%-30s %s/100tok\n" "directive.density" "$DIRECTIVE_DENSITY"

# ── 4. Section analysis ───────────────────────────────────
echo ""
echo "--- Section sizes (lines) ---"
# Extract sections between ## headers
awk '/^## / { if (header) printf "section[%-40s] %d lines\n", header, count; header=$0; count=0; next }
     /^# / { if (header) printf "section[%-40s] %d lines\n", header, count; header=$0; count=0; next }
     /./ { count++ }
     END { if (header) printf "section[%-40s] %d lines\n", header, count }' "$PROMPT"

# ── 5. Example count and type coverage ────────────────────
echo ""
EXAMPLE_COUNT=$(grep -c '^### Example' "$PROMPT" || echo 0)
printf "%-30s %s\n" "examples.count" "$EXAMPLE_COUNT"

echo "--- Example types covered ---"
grep '^### Example' "$PROMPT" | sed 's/### //'

echo ""
echo "--- Tool/action types mentioned ---"
echo -n "  tool: "; grep -c 'type=.tool.' "$PROMPT" || echo 0
echo -n "  agent: "; grep -c 'type=.agent.' "$PROMPT" || echo 0
echo -n "  relic: "; grep -c 'type=.relic.' "$PROMPT" || echo 0
echo -n "  feed: "; grep -c 'type=.feed.' "$PROMPT" || echo 0

# ── 6. Key behavior rules density ─────────────────────────
echo ""
echo "--- Behavior rule coverage ---"
echo -n "  think_then_act: "; grep -c 'Think.*then.*act\|plan.*before' "$PROMPT" || echo 0
echo -n "  read_results: "; grep -c 'Read.*result\|read.*carefully' "$PROMPT" || echo 0
echo -n "  one_at_time: "; grep -c 'one.*at.*time\|single.*turn' "$PROMPT" || echo 0
echo -n "  dont_repeat: "; grep -c 'repeat\|same.*parameter' "$PROMPT" || echo 0
echo -n "  stop_when_done: "; grep -c 'stop.*done\|final.*response\|enough.*information' "$PROMPT" || echo 0
echo -n "  turns_max: "; grep -c 'turns max\|iteration' "$PROMPT" || echo 0
echo -n "  error_handling: "; grep -c 'error\|fail\|alternative' "$PROMPT" || echo 0
echo -n "  tool_syntax: "; grep -c 'XML\|bare text.*invisible\|action.*tag' "$PROMPT" || echo 0

# ── 7. Template structure ratio ───────────────────────────
echo ""
echo "--- Code vs prose ratio ---"
CODE_LINES=$(grep -c '^\s*```\|^\s*<\|<action\|</action\|<result\|</result\|<response\|</response' "$PROMPT" || echo 0)
PROSE_LINES=$((LINES - CODE_LINES))
CODE_RATIO=$(python3 -c "print(round($CODE_LINES / $LINES * 100, 1))")
printf "%-30s %s\n" "ratio.code_lines" "$CODE_LINES"
printf "%-30s %s\n" "ratio.prose_lines" "$PROSE_LINES"
printf "%-30s %s%%\n" "ratio.code_pct" "$CODE_RATIO"

echo ""
echo "=== Done ==="
