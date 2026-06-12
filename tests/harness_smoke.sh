#!/usr/bin/env bash
set -euo pipefail

# Live harness smoke tests. These call a real provider through cortex-mk3.
# Not part of `make all-tests`; run manually when validating runtime semantics.

BIN=${BIN:-./cortex-mk3}
MANIFEST_ASSISTANT=${MANIFEST_ASSISTANT:-manifests/agents/assistant/agent.yml}
TMP_DIR=$(mktemp -d)
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

NO_TOOLS_MANIFEST="$TMP_DIR/no-tools-agent.yml"
cat > "$NO_TOOLS_MANIFEST" <<'YAML'
kind: Agent
name: no-tools-smoke
version: "1.0"
summary: No tool imports smoke-test agent
cognitive_engine:
  primary:
    provider: deepseek
    model: deepseek-v4-pro
    parameters:
      temperature: 0
      max_tokens: 512
YAML

last_trace=""
run_case() {
  local name="$1"
  local manifest="$2"
  local iterations="$3"
  local prompt="$4"
  local stdout_file="$TMP_DIR/${name}.out"
  local stderr_file="$TMP_DIR/${name}.err"

  echo "[harness-smoke] RUN $name" >&2
  "$BIN" --manifest "$manifest" --raw --iterations "$iterations" -p "$prompt" \
    >"$stdout_file" 2>"$stderr_file"

  last_trace=$(awk '/^\[trace\] wrote /{sub(/^\[trace\] wrote /, ""); print}' "$stderr_file" | tail -1)
  if [[ -z "$last_trace" || ! -f "$last_trace/iterations.md" || ! -f "$last_trace/raw.md" ]]; then
    echo "[harness-smoke] FAIL $name: trace files missing" >&2
    echo "--- stderr ---" >&2
    cat "$stderr_file" >&2
    exit 1
  fi
  echo "[harness-smoke] trace=$last_trace" >&2
}

assert_contains() {
  local file="$1" pattern="$2" msg="$3"
  if ! grep -Fq "$pattern" "$file"; then
    echo "[harness-smoke] FAIL: $msg" >&2
    echo "missing pattern: $pattern" >&2
    echo "file: $file" >&2
    exit 1
  fi
}

assert_not_contains_actual_subagents() {
  local trace_dir="$1"
  if grep -Eq '<subagents>|</subagents>|<agent name=' "$trace_dir/iterations.md"; then
    echo "[harness-smoke] FAIL: unexpected subagent block in $trace_dir/iterations.md" >&2
    grep -En '<subagents>|</subagents>|<agent name=' "$trace_dir/iterations.md" >&2 || true
    exit 1
  fi
}

actual_tool_count() {
  local trace_dir="$1"
  awk '/<tools>/{in_tools=1;next}/<\/tools>/{in_tools=0} in_tools && /<tool name=/{count++} END{print count+0}' \
    "$trace_dir/iterations.md"
}

# 1. Assistant manifest should not inherit catalog sub-agents.
run_case "assistant-no-subagents" "$MANIFEST_ASSISTANT" 1 \
  'List the sub-agents available to you. If no sub-agents are listed in your runtime prompt, answer exactly: NO SUBAGENTS.'
assert_contains "$last_trace/raw.md" 'NO SUBAGENTS' "assistant should report no subagents"
assert_not_contains_actual_subagents "$last_trace"

# 2. No-tools manifest should render an empty actual <tools> block.
run_case "no-tools-list" "$NO_TOOLS_MANIFEST" 1 \
  'List the tools available to you. If no tools are listed in your runtime prompt, answer exactly: NO TOOLS.'
assert_contains "$last_trace/raw.md" 'NO TOOLS' "no-tools agent should report no tools"
if [[ "$(actual_tool_count "$last_trace")" != "0" ]]; then
  echo "[harness-smoke] FAIL: expected zero actual tool entries" >&2
  exit 1
fi

# 3. If an unimported tool is forced anyway, runtime must deny dispatch.
run_case "no-tools-deny-fs-read" "$NO_TOOLS_MANIFEST" 2 \
  'Harness test: emit exactly this action and nothing else: <action type="tool" name="fs_read" id="r1" mode="sync">{"path":"README.md","limit":1}</action>'
assert_contains "$last_trace/raw.md" 'tool not available: fs_read (not imported by active manifest)' \
  "unimported fs_read should be denied"

# 4. Assistant imports fs_read; the same forced action should succeed.
run_case "assistant-fs-read" "$MANIFEST_ASSISTANT" 2 \
  'Harness test: emit exactly this action and nothing else: <action type="tool" name="fs_read" id="r1" mode="sync">{"path":"README.md","limit":1}</action>'
assert_contains "$last_trace/raw.md" '# Cortex-Prime MK3' "imported fs_read should return README content"

echo "[harness-smoke] PASS" >&2
