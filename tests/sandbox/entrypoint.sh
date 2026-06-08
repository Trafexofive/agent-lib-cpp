#!/bin/bash
# Sandbox entrypoint — runs all 7 harness scenarios inside the container.
# Uses zen (free, no API key) for "mock-like" testing.
set -euo pipefail

BINARY="${BINARY:-/usr/local/bin/cortex-mk3}"
HARNESS="${HARNESS:-/opt/cortex/harness/default.md}"
PROVIDER="${PROVIDER:-zen}"
MODEL="${MODEL:-big-pickle}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

passed=0
failed=0
total=0

run_test() {
    local id="$1"
    local desc="$2"
    local prompt="$3"
    total=$((total + 1))

    echo ""
    echo -e "${YELLOW}═══ [$id] ${desc} ═══${NC}"
    echo "  prompt: ${prompt:0:100}..."

    local output
    if output=$("$BINARY" \
        --raw \
        --harness-prompt "$HARNESS" \
        --provider "$PROVIDER" \
        --model "$MODEL" \
        --prompt "$prompt" 2>&1); then

        # Quick validation: must have XML tags
        local has_action=0 has_response=0
        echo "$output" | grep -q '<action ' && has_action=1 || true
        echo "$output" | grep -q '<response final="true">' && has_response=1 || true

        # Show preview
        echo "  --- output (first 6 lines) ---"
        echo "$output" | head -6 | sed 's/^/  | /'
        local lines=$(echo "$output" | wc -l)
        if [ "$lines" -gt 6 ]; then
            echo "  | ... (${lines} lines total)"
        fi
        echo "  ---"

        local issues=""
        [ "$has_action" = 0 ] && [ "$has_response" = 0 ] && issues="no XML tags"
        [ "$has_response" = 0 ] && [ "$has_action" = 0 ] && true  # action-only is ok

        if [ -n "$issues" ]; then
            echo -e "  ${RED}❌ $issues${NC}"
            failed=$((failed + 1))
        else
            local summary=""
            [ "$has_action" = 1 ] && summary="${summary}action "
            [ "$has_response" = 1 ] && summary="${summary}response"
            echo -e "  ${GREEN}✅ [$summary]${NC}"
            passed=$((passed + 1))
        fi
    else
        echo -e "  ${RED}❌ binary failed${NC}"
        echo "$output" | tail -5 | sed 's/^/  | /'
        failed=$((failed + 1))
    fi
}

echo "╔══════════════════════════════════════════╗"
echo "║  Cortex MK3 — Harness Scenario Runner   ║"
echo "╠══════════════════════════════════════════╣"
echo "║  binary:   $BINARY"
echo "║  harness:  $HARNESS"
echo "║  provider: $PROVIDER"
echo "║  model:    $MODEL"
echo "╚══════════════════════════════════════════╝"

# ── Scenario 1: Protocol Smoke ──
run_test "01" "Protocol smoke — identity" \
    "Who are you and what protocol do you speak? Respond directly."

run_test "02" "Protocol smoke — simple math" \
    "What is 7 times 9? Answer concisely."

# ── Scenario 2: Multi-turn Tools ──
run_test "03" "Multi-turn — list directory" \
    "List the files in the current directory."

run_test "04" "Multi-turn — ls → read → search" \
    "First list files in src/, then read src/core/agent.hpp (first 5 lines), then search for 'TODO' in src/."

# ── Scenario 3: Sub-agent Delegation ──
run_test "05" "Sub-agent — delegate build" \
    "Delegate a build task to the builder agent: compile and report errors."

# ── Scenario 4: Error Recovery ──
run_test "06" "Error recovery — file not found" \
    "Read /nonexistent/ghost.txt. If it fails, list the directory and suggest alternatives."

run_test "07" "Error recovery — bad command" \
    "Run 'invalid_command_xyz --flag'. Report the error gracefully."

run_test "08" "Error recovery — permission denied" \
    "Try to read /etc/shadow. If access is denied, try /etc/hostname instead."

# ── Scenario 5: Feed Polling ──
run_test "09" "Feed polling — CI status" \
    "Poll the ci-status feed for the 5 most recent jobs. Report pass/fail counts."

# ── Scenario 6: Relic Integration ──
run_test "10" "Relic — database health" \
    "Check if the postgres_main database is reachable by querying 'SELECT 1'."

# ── Scenario 7: Stress Cycling ──
run_test "11" "Stress — analyze project" \
    "Analyze this project: list src/, find all TODO comments, read the first file with TODOs, pin it to context, and write a summary."

run_test "12" "Stress — complex orchestration" \
    "Search for 'ERROR' in logs/, list config/agents/, read the assistant config, check the postgres relic, and report everything."

# ── Summary ──
echo ""
echo "╔══════════════════════════════════════════╗"
echo -e "║  RESULTS: ${GREEN}${passed} passed${NC}, ${RED}${failed} failed${NC}, ${total} total  ║"
echo "╚══════════════════════════════════════════╝"

[ "$failed" -eq 0 ] && exit 0 || exit 1
