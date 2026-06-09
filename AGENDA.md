# MK3 AGENDA

Living document tracking Cortex-Prime MK3 agent-lib status, priorities, decisions, and goals.

---

## 1. Current Status

| Area | Status |
|------|--------|
| **Codebase refactor (Jun 9)** | **Phase 1-2 deployed** — agent.cpp split (3 files), ManifestYaml extracted (mini_yaml.hpp) |
| Harness protocol (XML tags) | Deployed — self-checks, failed-turn mirroring, closing reminder |
| Iteration loop | Deployed — history cap enforced, JSON synthesis removed, bare-text reminder |
| Tool execution | Deployed — `timeout` enforcement, all 11 built-ins registered |
| Built-in tools | Deployed — formatted JSON schemas, rich result attrs (ms/bytes/exit) |
| Tool hot-reload | Deployed — `reload_manifests` tool, disable/enable builtins |
| Workflow engine | Deployed — code-review workflow, step params, list-item parsing fixed |
| Result rendering | Deployed — plain-text body, `<result ok="true" ms="12" bytes="2048">` |
| System prompt | Deployed — section descriptions, consistent indentation, no CDATA |
| Session persistence | Deployed — prefix doubling fixed, session load/save roundtrip |
| Parser streaming | Deployed — `</response>` detection, `final=true` propagation, 9/9 tests |
| CLI flags | Deployed — all short flags working (-s, -E, -H, -y, -S, -R, etc.) |
| HTTP retry | Deployed — iterative loop with exponential backoff |
| Metadata headers | Deployed — `✓ 234ms exit:0 12.3KB` per action |
| raw.md / iterations.md dumps | Deployed — PROMPT + RESPONSE + LLM RAW OUTPUT + TOOL RESULTS |
| Crash: `free(): invalid pointer` | Fixed — threads joined on destructor |
| **Spinner/live typing during LLM** | **Blocked** — `curl_easy_perform` blocks main thread |
| **LLM protocol compliance** | **~60%** — deepseek-chat ceiling, bare text ~40% of turns |
| **Persona separation** | **Deployed** — assistant.md + decoupler.md pure behavioral, no protocol leaks |

---

## 2. Priority Queue

1. **Async LLM** — non-blocking HTTP (curl_multi or threaded client). Last remaining architectural blocker.
2. **Runtime compliance enforcement** — strict XML mode: reject turns with bare text, inject error result. Complements ~60% prompt-level ceiling.
3. **Manifest ecosystem** — bi-directional import, remote resolving, disable unsupported builtins for auto-readonly mode.
4. **Testing** — automated end-to-end tests: crash on exit, pipeline roundtrip, self-improvement scenarios.

---

## 3. Architecture Decisions

### Threaded tool execution
Script tools run in `std::thread` with `popen()`. Output streams to `PendingTool::output` (mutex-protected). `harvestPendingTools()` called per SSE token to push incremental results. Threads joined on Agent destruction.

### Incremental ProtocolView renderer
`render(width)` only processes NEW actions/results since last call. Tracks `lastAction_`/`lastResult_` cursors. Appends to `cached_lines_`. Cache resets on result update (progressive streaming).

### Foreground-only ANSI in colored blocks
`ansi::reset()` = `\033[0m` kills background. Added `fgReset()` = `\033[39m\033[22m\033[24m` — resets foreground/bold/underline only, preserves background. All rendering inside `bgAction()/bgOk()/bgErr()` blocks uses foreground-only formatting.

### Harness single responsibility
- **Harness** (default.md): protocol only — XML format, parallel/pipelines, error recovery, stop conditions. No tool catalogs.
- **Tool configs** (tool.yml): tool-specific schemas, examples, constraints.
- **System prompt** (agent.yml persona): agent personality, available tools list.

### Bare text tolerance
LLM output without `<action>` or `<response>` tags is treated as the final response. This prevents iteration loops from turning 1-turn queries into multi-API-call spirals.

---

## 4. Performance Targets

| Metric | Target | Current |
|--------|--------|---------|
| Spinner update rate | 30fps during LLM call | 0fps (blocked by curl) |
| Time to first token display | <100ms after SSE arrives | ~1ms (callback fires inline) |
| Render frame time | <500µs | ~200-500µs estimated |
| Simple "ping" roundtrip | <2s total | ~3-8s (LLM + network) |
| Tool result stream latency | <50ms from output line to display | ~1ms (harvest per token) |
| Typing responsiveness during streaming | Instant | Blocked (curl_easy_perform) |

---

## 5. Known Debt

| Debt | Impact | Mitigation |
|------|--------|------------|
| `curl_easy_perform` blocks main thread | Spinner freezes, can't type during LLM | Ctrl+C responsive via `CURLOPT_XFERINFOFUNCTION`. Full fix: async LLM (#1 priority) |
| No thread-safe protocol vectors | Potential data race when async LLM added | Acceptable now — single-threaded. Must add mutex before async |
| LLM emits bare text outside tags | Extra API calls, garbled raw.md | Harness tuning needed. Bare text = response mitigation limits impact |
| No automated tests | Regressions manually detected | Manual testing for now. Test harness needed |
| Markdown renderer handles partial text poorly | Live MD can be garbled during streaming | Acceptable — resolves on completion. Lazy rendering option exists |

---

## 6. Action Rendering Spec

Goal: pi-agent quality tool display with MK3-specific protocol awareness.

### Layout (per action block)
```
  ⚙ exec#e1                  ← bg: dark blue-grey (#1E1E28)
      command:  ls -la
  ✓ 234ms exit:0 12.3KB      ← bg: dark green (#19231E) or dark red (#281919)
  README.md                   ← output lines, dimmed
  src/
  Makefile
  ---                         ← dimmed separator
```

### States
- **Pending** (yellow): `⏳ exec#e1 ...` — action queued, not started
- **Running** (cyan): `⠼ exec#e1 ...` — animated spinner, partial output streaming
- **Done** (green): `✓ exec#e1 234ms exit:0 12.3KB` — full output, metadata
- **Failed** (red): `✗ exec#e1 1.2s exit:1 ERROR` — error details

### Progressive streaming
Results append line-by-line. Each `harvestPendingTools()` call pushes new lines with same action ID. Renderer detects ID match and appends to existing block. Cache resets so block re-renders with new content.

### Builtin special rendering
- `exec`/`bash`: full output, no truncation
- `list`/`ls`: full listing, dimmed
- `read`/`write`/`edit`: first line + metadata
- JSON results: YAML-style key:value display

---

## 7. Harness Philosophy

### What goes where

| Concern | Location | Example |
|---------|----------|---------|
| XML protocol format | `config/harness/default.md` | `<action type="tool" name="exec" id="e1">...</action>` |
| Parallel execution rules | `config/harness/default.md` | `depends_on`, concurrent actions |
| Error recovery table | `config/harness/default.md` | File not found → list directory |
| When to stop searching | `config/harness/default.md` | "One extra lookup fine, three too many" |
| Tool names + descriptions | System prompt (from manifest `import.tools`) | `<tool name="exec" description="Run shell commands"/>` |
| Tool parameter schemas | `tool.yml` → `ToolDef::toXml()` | JSON schema, required params, examples |
| Tool-specific constraints | `tool.yml` description field | Sandbox, allowed paths, timeout |
| Agent personality | `system-prompts/agent.md` | "You are a helpful assistant" |
| Available tools list | System prompt (`buildSystemPrompt`) | Gated by `tools_` (manifest imports) |
| Available relics/feeds | System prompt | Gated by `import.relics` / `import.feeds` |

### Principles
- **Harness teaches the protocol, not the tools.** The LLM learns HOW to call tools from the harness, but WHAT tools exist from the system prompt.
- **Tools self-document.** Each tool's `tool.yml` has description + schema. These inject into the system prompt, not the harness.
- **Bare text is tolerated but not encouraged.** The harness says "wrap everything in XML." But if the LLM doesn't, we treat bare text as the response rather than looping forever.
- **Non-final `<response>` = thinking.** The LLM can emit `<response>I should check X</response>` alongside actions. It appears briefly then vanishes (context management TBD).

---

## 8. Test Plan

### Manual tests (per session)
- [ ] `/exit` → no crash (`free(): invalid pointer`)
- [ ] Simple prompt ("ping") → response within 5s, no crash
- [ ] Tool prompt ("ls") → action block renders with bg colors, output visible
- [ ] Multi-turn ("check git status and read README") → both actions render, response follows
- [ ] Ctrl+C during streaming → prompt cancels cleanly
- [ ] `raw.md` contains `<action>`, `<result>`, `<response>` tags
- [ ] `iterations.md` shows per-turn prompts

### Automated tests (to build)
- [ ] Crash on exit (valgrind/ASAN)
- [ ] Protocol roundtrip (parser feed → events → dispatch)
- [ ] Renderer snapshot (known input → known output)
- [ ] Harness parsing (template injection → valid XML)
- [ ] Tool execution timeout (runaway process killed)

---

*Last updated: 2026-05-16 | 61c3a7e*
