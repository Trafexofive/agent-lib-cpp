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
| Result rendering | Deployed — CANON shape `<result status="ok|error|timeout|protocol_error" …>` |
| System prompt | Deployed — section descriptions, consistent indentation, no CDATA |
| Session persistence | Deployed — prefix doubling fixed, session load/save roundtrip |
| Parser streaming | Deployed — `</response>` detection, `final=true` propagation, 9/9 tests |
| CLI flags | Deployed — all short flags working (-s, -E, -H, -y, -S, -R, etc.) |
| HTTP retry | Deployed — iterative loop with exponential backoff |
| Metadata headers | Deployed — `✓ 234ms exit:0 12.3KB` per action |
| raw.md / iterations.md dumps | Deployed — PROMPT + RESPONSE + LLM RAW OUTPUT + TOOL RESULTS |
| Crash: `free(): invalid pointer` | Fixed — threads joined on destructor |
| **Spinner/live typing during LLM** | **Shipped (TUI)** — experimental TUI runs the agent on a worker (`repl.hpp`); curl blocks the worker, not the Engine. Typing/stream drain work. Remaining: free-tier TTFT, honest phase/footer (2026-08-19). |
| **LLM protocol compliance** | CANON shipped 2026-07-10. **Do not quote 60% as current** — remeasure before citing. |
| **Global agent selection / any-CWD** | **Shipped** — `manifests/` + `config/agents/`; bare `-m` manager + ownership trees |
| **TUI (builtin / `src/tui`)** | **Oracle only** — `--tui legacy`. Do not invest. |
| **inkcell TUI** | **Default product path** on `feat/inkcell-agentshell`. Native App. Layout target: tetris/aart convention (`app/` `views/` `data/` `assets/` `components/`). |
| **Tool / exec hang** | **Hard rule:** every subprocess goes through `process::run` with a wall clock. `popen` leftover is a bug. Caps: exec 1–600s, script tools 1–600s, feed git 2s, clipboard 1.5s, builds 120s. |
| **Persona separation** | **Deployed** — harness = protocol, persona = behavior, tools = schemas |

---

## 2. Priority Queue

### Manifest `compaction` — **SHIPPED (MVP)**

Canonical: `docs/manifests/compaction.md`  
Hybrid of minimal + recommended + profile sugar.  
`max_turns_per_cycle` default **15** (dumb seatbelt not reclamped every turn).

**Still open (phase 1b+):** LLM summarize, artifact-graph archive sink, subagent child_before_return enforcement, real tokenizer, `/compact` slash command.

**Artifact (design ancestry):** `compaction-manifest-drafts-v0`

> **SHORT-TERM HARD GOAL (added 2026-07-17):** Get Cortex-Prime MK3 to **pi-level daily-driver capability** — the threshold where it can be trusted as a primary agent harness for real work, not just demos. Until that threshold clears, use Cortex instances as **sub-agents spawned from pi** (test phase): pi orchestrates, Cortex executes delegated tasks, failures feed back into gap closure. Full gap analysis + integration plan: artifact `cortex-pi-level-gap-analysis`.
>
> **Daily-driver bar (must clear all):** (a) protocol compliance ≥90%, (b) async LLM (non-blocking stream + responsive composer), (c) sub-agent delegation, (d) artifact graph + plans, (e) context economy (compaction/squeezer), (f) streaming UX smoothness.

0. **Protocol CANON burn** — **shipped 2026-07-10**. Authority: `docs/protocol/CANON.md`. Remeasure compliance against honest contract.
1. **Global agent / manifest selection (any-CWD UX)** — **shipped** (`manifests/` only, bare `-m` manager, ownership trees).
2. **Inkcell full migration** — execute `docs/INKCELL_MIGRATION.md` (skeleton → bridge → AgentShell → protocol widgets → pickers → cutover).
   - Selection interface: CLI (`--agent <name|path>`, fuzzy list), interactive picker at startup / slash command (`/agent`, `/manifest`).
   - Resolve harness/system/persona/tools relative to **manifest home**, not process CWD.
   - Document install layout + env vars; keep one-shot `-m path/to/agent.yml` as escape hatch.
2. **Inkcell TUI (DECIDED — default)** — `src/ui/` is the product. Builtin `src/tui` is oracle. Remaining work is structure + QoL (`docs/AUDITS/REPORTS/2026-08-16-chat-ux-backlog.md`), not a substrate bake-off.
3. **Async LLM** — non-blocking HTTP (curl_multi or threaded client). Unblocks spinner + typing during stream.
4. **Runtime compliance enforcement** — strict XML mode already partially in CANON path; harden protocol_error injection + metrics.
5. **Manifest ecosystem** — bi-directional import, remote resolving, disable unsupported builtins for auto-readonly mode.
6. **Testing** — automated end-to-end tests: crash on exit, pipeline roundtrip, self-improvement scenarios.
7. **TUI select-block primitive** — *after* TUI substrate decision. Keyboard-select rendered blocks and jump into/out of action scopes (parent ⇄ sub-agent). Generic: block identity, parent/child, focus stack; later copy/pin/inspect/jump.
8. **Dashboard 10x + no-args default** (2026-07-17) — make the dashboard a real operator control surface (live metrics, actionable controls, monitoring), and make `cortex-mk3` with no args open the dashboard instead of erroring. See artifact `cortex-pi-level-gap-analysis` §5.

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

### Bare text policy (CANON §2 — STRICT)
Bare text does **not** complete the turn. Runtime injects a protocol correction and continues.
Only `<response final="true">` completes normally. Authority: `docs/protocol/CANON.md`.

---

## 4. Performance Targets

| Metric | Target | Current |
|--------|--------|---------|
| Spinner update rate | ~30fps while running (Clock.mark) | OK on experimental TUI |
| Time to first token display | <100ms after SSE arrives | coalesce ≤16–33ms + TTFT |
| Render frame time | <500µs | ~200-500µs estimated |
| Simple "ping" roundtrip | <2s total | network-bound |
| Tool result stream latency | <50ms from output line to display | OK |
| Typing responsiveness during streaming | Instant | OK — worker holds curl |

---

## 5. Known Debt

| Debt | Impact | Mitigation |
|------|--------|------------|
| `curl_easy_perform` on **worker** (TUI) | Not a UI freeze | Optional curl_multi later for multi-stream; not blocking daily driver |
| No thread-safe protocol vectors | Data race **when** async LLM lands | Mutex before async, not before |
| LLM emits bare text outside tags | Extra API calls until correction | CANON §2 retry. Remeasure — do not cite old 60% |
| Tests exist (`test-parser`, `test-protocol`, `test-ui-model`, session/epoch) | `test-ui-model` has known expectation drift | Fix tests or product; do not ignore red |
| Markdown renderer handles partial text poorly | Live MD garbled mid-stream | Acceptable until complete |

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
- **Bare text does not complete.** Harness and runtime agree (CANON §2): correction + retry, not silent success.
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

*Last updated: 2026-08-18 | hang-kill (process::run only) + chat contrast/composer + agenda resync*
