#!/usr/bin/env bash
set -euo pipefail

bin="${1:-./cortex-mk3}"

make test-ui-model >/dev/null
make test-ui-view >/dev/null
make test-chat-scene >/dev/null
tests/tui/ui_architecture_smoke.sh
tests/tui/experimental_chat_smoke.sh "$bin"
tests/tui/repl_parity_smoke.sh "$bin"

echo "chat business parity smoke passed"
