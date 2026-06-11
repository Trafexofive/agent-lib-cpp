# MK3 agent-lib-cpp — Continuation Prompt

You are continuing work on Cortex-Prime MK3, a C++ agent runtime.
This is a handoff from ~42 commits across a tuning/building/testing sprint.

## PROJECT

```
Repo:  git@github.com:Trafexofive/agent-lib-cpp
Branch: master (latest: d3d873a)
Binary: ./cortex-mk3
Build:  make cortex-mk3
Test:   make test-parser (9/9) | make test-protocol (23/25, 2 expected fails)
```

## ARCHITECTURE — STRICT LAYER SEPARATION

```
Harness (--harness / config/harness/default.md):  Protocol ONLY. XML format, self-check, tags, rules.
Persona (--system):                               Behavioral/tonal framing ONLY. No tools, no protocol, no examples.
Tools (tool.yml):                                 Auto-loaded schemas. Descriptions at source.
Agent (agent.yml):                                Which persona, which tools, which model.
```

**This separation is non-negotiable.** The user will reject any work that mixes layers.

## KEY FILES

```
src/core/agent.cpp          828l  — constructor, core loop, prompt building
src/core/agent_tools.cpp    343l  — dispatch, script exec, tool mgmt, reload, session persist, subagents
src/core/agent_session.cpp  107l  — save, load, dump, clear, undo
src/core/agent.hpp          125l  — single header for all 3 cpp files
src/core/manifest_loader.hpp 501l  — ManifestLoader (loadAgentConfig, loadToolManifest, loadRelicConfig)
src/core/mini_yaml.hpp      129l  — shared YAML parser (ManifestYaml)
src/core/manifest_autoload.hpp 144l — recursive manifest directory scanner
src/core/types.hpp           — AgentConfig, ToolDef, AgentContext
src/relics/builtin_relics.hpp  — session_journal, state_checkpoint + HTTP relic dispatch
src/workflows/workflow_engine.hpp — WorkflowEngine (load, toXml, step parsing)
src/providers/generic_openai.cpp — CURL retry, JSON parsing, streaming
config/harness/default.md    — protocol enforcement prompts
main.cpp                     1218l — CLI, config loading, manifest autoload
```

## CURRENT CAPABILITIES

| Feature | Status |
|---------|--------|
| 14 built-in tools (exec, list, grep, fs_read, fs_write, json, web_fetch, ask_tool, context_pin/peek/unpin, reload_manifests, disable/enable_builtin) | ✅ deployed |
| Synchronous script tool execution (returns actual output, no PendingTool stubs) | ✅ T17 fixed |
| Session persistence for dynamic tools (tools survive process restarts) | ✅ T04 fixed |
| Relic dispatch (built-in + HTTP fallback for Docker relics) | ✅ T12, T20 fixed |
| Kind-based manifest filtering (top-level `kind` field checked) | ✅ T16 fixed |
| CURL transient error retry (RECV_ERROR, SEND_ERROR, etc.) | ✅ deployed |
| Workflow engine (step parsing, params rendering, list-item YAML fixed) | ✅ deployed |
| Disable/enable builtins at runtime | ✅ deployed |
| Hot-reload tool manifests mid-session | ✅ deployed |
| Multi-generation self-improvement (agent creates + improves + chains tools) | ✅ verified |
| Production-scale manifest module (cpp-refactor-suite: 19 manifests, 1074 YAML) | ✅ in modules/ |
| `--iterations/-X <n>` CLI flag | ✅ deployed |

## REMAINING HIGH-ROI WORK

```
T03  — manifest max_iterations should override AgentConfig automatically (not just via CLI flag)
T09  — workflow step variable resolution (${step.output} piping)
T13  — feed auto-refresh (refresh_seconds parsed but never triggers)
T18  — workflow depends_on implementation
T19  — custom tools appear twice in system prompt (dedup)
T06  — system prompt cached mid-turn, doesn't rebuild after reload
T07  — server mode (--serve)
T21  — enable_builtin doesn't restore actual tool schemas (re-registration missing)
T25  — feed engine requires runtime+entrypoint, declarative feeds fail
T26  — disable_builtin param name mismatch (LLM sends "tool" not "name")
T28  — path traversal not blocked in manifest entrypoint paths
```

## BUILD & TEST

```bash
# Build
make cortex-mk3

# Full clean (header changes need this)
rm -rf build && make cortex-mk3

# Smoke test
make smoke

# All tests
make all-tests

# Run agent
./cortex-mk3 --raw --harness config/harness/default.md --provider deepseek --model deepseek-chat -p "hello"

# With custom iterations
./cortex-mk3 --iterations 30 --raw --manifest-dir /path/to/manifests -p "do stuff"

# Protocol test
make test-protocol

# Install system-wide
sudo make install
```

## GOTCHAS

1. **Header changes require full `rm -rf build` rebuild.** Stale .o files with different class layouts = segfault. The makefile dependency tracking isn't reliable for header changes.

2. **deepseek-chat ~60% protocol compliance** — the LLM produces bare text ~40% of turns regardless of harness quality. This is model-level, not code-level. deepseek-v4-pro has ~100% compliance but is slower and more expensive.

3. **Never mix harness/persona/tools layers.** If you add a tool description to the harness prompt, the user will notice. Tools belong in tool.yml only.

4. **iterations.md is the source of truth for debugging.** The user reads iterations.md to verify changes. All prompt improvements must be VISIBLE there.

5. **AgentConfig iterationCap defaults to 20.** Override via `--iterations N` or manifest `max_iterations`.

6. **The user prefers:** atomic commits, ask_cards for alignment, testing everything, never narrating, building aggressively.

## WORKING WITH THE USER (mlamkadm)

- Never ask open-ended questions — use ask_cards or just execute
- Never stop organically — keep slamming or ask_cards to align
- Test everything, verify with actual output, never assume
- Commit each change atomically, push immediately
- The user is self-sufficient — reports, reloads, operates independently
- "10x the scale" — always push for bigger/badder tests
- "child's play" — the user rejects surface-level testing
- "The Great Work Never Ends" — more features, more scale, more stress

## DEEPSEEK-CHAT COMPLIANCE CEILING

The core challenge: the LLM ignores the protocol ~40% of turns. The harness
(self-checks, failed-turn mirroring, closing reminders) raises compliance from
~30% to ~60% but plateaus there. Runtime enforcement (bare-text detection,
injecting error results) catches some cases. The remaining gap is model-level.

deepseek-v4-pro achieves near-100% compliance but is slower and costs more.
