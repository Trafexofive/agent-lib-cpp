#!/bin/bash
# sandbox test runner — run an agent in a Docker container and validate output
# Usage: ./tests/sandbox/run.sh [agent] [--prompt "text" | --file prompts.txt]
set -euo pipefail

AGENT="${1:-assistant}"
shift || true

IMAGE="cortex-mk3-sandbox"
PROMPT=""
PROMPT_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prompt) PROMPT="$2"; shift 2 ;;
        --file)   PROMPT_FILE="$2"; shift 2 ;;
        --build)  BUILD_FIRST=1; shift ;;
        *) echo "Unknown: $1"; exit 1 ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# Build image if needed
if [[ -n "${BUILD_FIRST:-}" ]] || ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "=== Building $IMAGE ==="
    docker build -t "$IMAGE" -f "$ROOT/tests/sandbox/Dockerfile" "$ROOT"
fi

MODEL="${CORTEX_MODEL:-deepseek-chat}"
PROVIDER="${CORTEX_PROVIDER:-deepseek}"

run_prompt() {
    local prompt="$1"
    echo ">>> $prompt"
    docker run --rm \
        -e DEEPSEEK_API_KEY \
        -v "$ROOT:/workspace:ro" \
        "$IMAGE" \
        --provider "$PROVIDER" \
        --model "$MODEL" \
        --harness-prompt "/opt/cortex/manifests/agents/$AGENT/system-prompts/assistant.md" \
        --prompt "$prompt" \
        --raw
    echo
    echo "---"
}

if [[ -n "$PROMPT" ]]; then
    run_prompt "$PROMPT"
elif [[ -n "$PROMPT_FILE" ]]; then
    while IFS= read -r line; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        run_prompt "$line"
    done < "$PROMPT_FILE"
else
    # Default read-only tests
    run_prompt "List the files in the current directory."
    run_prompt "Search for 'Agent' in src/core/agent.hpp."
    run_prompt "Read src/core/types.hpp and list the structs defined in it."
fi
