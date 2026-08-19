# MK3 Live Tickets

Open, in-progress, and recently-resolved tickets for the MK3 codebase. Anything
historical and not part of the live backlog lives in
[`docs/archive/`](./archive/).

---

## Board — 2026-08-19 harness / daily-driver (live dumps)

Evidence dumps under `.cortex/dev/ephemeral-*` and
`docs/AUDITS/REPORTS/2026-08-19-*.md`. Update status when a slice lands + dump-verifies.

### Shipped (do not regress)

| ID | Item | Evidence |
|----|------|----------|
| T-SOT | History `<result>` full SoT (no 2KB compact inject) | `ephemeral-1801446`, `67af8c4` |
| T-DIET | Slim tool catalog on iter≥2 | tools[0]=9.1k full → tools[1+]=2.2k slim (`1905044`, `1923837`) |
| T-JSON | invalid_json before hollow gate | `test-parser` 24+ |
| T-HOLLOW-BRACES | Treat tool body `{}` as hollow (not content) | parser + unit test |
| T-EMPTYCHAT | iter≥2 user msg + 400 retry | prior commits |
| T-FOOTER | 6-row live footer instrument | `de93e55` |
| T-CANVAS | Ctrl-O stream/compact/canvas + timelineRows | body views |
| T-BRAND | README + .desktop + master force | `bd8d2cd`… |
| T-TIMEOUT-CANCEL | SIGTERM/wall → TIMEOUT harness not CANCEL | `ephemeral-1953397` |
| T-DEFAULT-PROMPT-BLOAT | fat catalog cards-only (tools ~4.3KB) | `ephemeral-1963512` 4329B |
| T-SUBAGENT-WALL | delegated iter cap 32 + action timeout wall | agent.cpp seatbelt |

### Open — P0

| ID | Ticket | Symptom (dump) | Acceptance |
|----|--------|----------------|------------|
| **T-DUP-STREAM** | Model streams same action id twice in one gen (full+full or hollow+full). History stores both tags. | `1905044` list×2; `1923837` list×2; pre-fix grep `{}`+full | One execute; optional collapse duplicate tags in rendered history; hollow `{}` never executes |
| **T-DUP-STREAM** | (still open) Model streams same action id twice | history dual tags | collapse / single execute |

### Open — P1

| ID | Ticket | Notes |
|----|--------|-------|
| **T-ONE-WORKER** | PE shipped; need live proof one agent only on light scout | PE in `harness/default.md`; re-run C2 with ≥10min wall |
| **T-COMPACT-CANVAS** | Compact/canvas craft under live TUI | Rows wired; polish selection/edges |
| **T-WAIT-JOIN** | PE prefers wait/join over sleep | Tool exists; PE line only |
| **T-HISTORY-DEDUP-TAGS** | Agent history line keeps duplicate `<action id=same>` text | Display/export cleanup |
| **T-CLI-HEADLESS** | Bare `--session` opens hub TUI | still open |
| **T-CLI-M-PROVIDER** | `-m` vs `--provider` free tokens | **shipped** `3008514` |
| **T-ONE-WORKER** | live one agent scout | **shipped** dump `2167980` |
| **T-MANIFEST-SEATBELT** | stage/coder / sub max_iterations still huge | Don’t commit operator WIP blindly |

### Open — P2

| ID | Ticket |
|----|--------|
| **T-PARSER-STALE-DOCS** | Old TICKETS POC section below is archival noise — keep separate |
| **T-PRODUCT-CHROME** | Beyond README/.desktop |
| **T-EXPORT-ORDER** | Chat export vs protocol event order drift |

### Live dump log (append-only)

| Dump | Model | Result |
|------|-------|--------|
| `ephemeral-1801446` | flash | SoT full fs_read; trunc=0 |
| `ephemeral-1897888` | flash | diet slim on continue |
| `ephemeral-1905044` | flash | multi-tool OK; dup list tags; trunc=0 |
| `ephemeral-1905607` | flash | bare→PROTOCOL nudge→list→final OK |
| `ephemeral-1910320` | flash `-m default` | one coder agent started; **timeout→CANCEL race**; tools 52KB |
| `ephemeral-1923837` | **pro** | list→final OK; diet slim iter2; dup list tags |
| `ephemeral-1953397` | flash | **TIMEOUT** harness (not CANCEL) on SIGTERM |
| `ephemeral-1963512` | flash default | tools_len **4329** (was 52k) |
| `ephemeral-2167980` | flash default | **ONE worker** coder #scout1 45s ok=true bytes=3033 final |

---

## Authoritative source (legacy POC audit)

The current open audit findings live as an artifact and are kept in sync as
slices ship:

- `agent-lib-cpp-poc-stub-audit-2026-06-20` (artifact `art-mqmo4hl5-w3npoz`)

When the audit changes, this file should be updated to reflect the new
priorities — not the other way around.

## Live open tickets

The list below carries only items the user has explicitly prioritized. Each
slice gets its own commit so a bisect stays clean.

### Operator UX — global agents + TUI substrate

- **Global agent / manifest selection (any CWD)** — **shipped 2026-07-10/11**.
  `manifests/` only; bare `-m` manager; ownership trees; binary-adjacent catalog.
- **Inkcell full migration (started 2026-07-11)** — authoritative plan:
  `docs/INKCELL_MIGRATION.md` + artifact `art-mrflxfd4-gupoyt`.
  Phase 0 skeleton + AgentBridge stub shipped. Strangler path `--tui inkcell|legacy`.
  Next: wire Agent callbacks → bridge → ScrollView. Do not put protocol into inkcell.
  Gate: adopt / keep-and-rewrite / hybrid. Block further TUI features (select-block,
  chrome) on this decision.

### POC / stub cleanup — first batch (in progress)

| Slice | Status | Note |
|---|---|---|
| Remove no-op `working_directory.touch` demo feed tool | done | `d40be0b` |
| Hard-error unknown dotted feed tools | done | `4f79ca8` |
| Parse `feed.yml` `tools:` block into spec store | done | `b185281` |
| Archive stale `docs/TICKETS.md` | done | this commit |

### POC / stub cleanup — second batch (queued)

- **Move feed runtime to `process::run`** with per-call env map, timeout, output
  caps, exit status, and structured result. (P1 in the audit; depends on
  the manifest-tools parse path shipped in this batch.)
- **Add manifest-declared feed tool runtime** that turns the parsed specs from
  `feedManifestTools()` into real handlers using the new process substrate.
  Expose merged specs in the prompt only after the handler is wired.
- **Workflow agent modifiers** — propagate `ephemeral` / `dump_context` through
  `WorkflowRuntime::executeAgent` so workflows can request response-only or
  trace behavior parity with direct agent actions.

### POC / stub cleanup — third batch (backlog)

- **Unify relic runtime substrate** with `process::run` and shared manifest
  parser. Today `DockerRelicDispatcher` is a separate substrate with its own
  `popen` paths and a `Simple YAML key: value parser`.
- **Fix remote-relic endpoint mismatch** (`endpoint` is treated as full URL;
  callers pass a bare path).
- **Decide on `cortex-mk3 serve` UX** — either exec `cortex-mk3-server` or
  remove the documented command. Today it prints a stub message and exits 1.
- **Server create-agent `max_tokens` bug** — checks `max_tokens` (snake) but
  reads `maxTokens` (camel).
- **Manifest `fallback` provider/model** is parsed into `AgentConfig` but has
  no runtime execution path. Either implement or remove from schema.
- **Delete or route old `Tool::executeScript`** shell runner to `process::run`
  so the hardened substrate covers every tool execution path.

## Live session debug — inkcell-1786357930926 (2026-08-10)

Investigated the latest live test (read-only AI-slop audit across the inkcell
repo). The harness surfaced a critical bug class, now fixed:

- **Thought-only final lost to stale non-final response** — the model did the
  entire audit inside `<thought>`, forgot `final="true"`, and a stale
  non-final `<response>Scanning structural patterns now...</response>` from an
  early iteration won as the turn's answer (`responseOutput_` accumulated
  across iterations). The actual deliverable was invisible to the operator.
  **Fixed in `20c9625`** — per-iteration `responseOutput_`/`thoughtOutput_`
  reset, plus bounded thought-to-final recovery at cap (surfaces the last
  substantive >120-char `<thought>` if no real final was emitted).
- **Pointless empty `exec {}` at finalization** (model behavior, not bug) — the
  model emitted two `<action type="tool" name="exec">{}</action>` with empty
  bodies; the tool correctly errored (`command is required when shell=true`).
  Wasteful but correctly handled.

Open after this session:
- **Reasoning models put finals in thoughts frequently** — the bounded recovery
  handles it, but consider strengthening the finalization prompt so models
  re-emit `final="true"` reliably (fewer cap-promotes).
- **`test-ui-model` 7 pre-existing failures** — dashboard nav (3), dev-gated cmd
  classification (3: `/prompts` `/dump-prompt` `/cp-raw` called without devMode),
  dynamic-completion (1). Unrelated to the above; fix the test expectations.
- The session itself confirmed repo-root AI artifacts (root `iterations.md`/
  `raw.md`/`history.md`, `.cortex/`, `.artifacts/`) are a recurring slop class —
  add `.gitignore` entries (see repo-root cleanup track).

## Recently resolved

Tickets that were on the historical doc but are now closed get a one-line
note here. Anything more than one line belongs in the commit message and the
audit artifact.

- `FE04` (setenv after popen in feed script runner) — historical ticket
  referenced code paths that have since been reorganized; the call order was
  corrected in a later refactor. The follow-up substrate work above will
  remove the global-env path entirely.
- `AC14` (provider hardcode in `Agent::saveSession`) — fixed; session
  metadata now uses the actual provider from the active config.

## Adding tickets

Add a row in the appropriate batch. The audit artifact is the source of
truth — when you add a row here, link the slice commit when it lands and
remove the row from "Live open" if the slice completes.

### Agent manifest expansion — from `bloated` prototype (queued, design complete)

Each slice has a design in `docs/manifests/context-and-sandbox-design.md`. The `bloated` agent at `staged-manifests/agents/bloated/` is the forward-looking demo of what the full surface area looks like.

| Slice | Status | Note |
|---|---|---|
| `context:` block: 6 new fields (`history_mode`, `on_protocol_violation`, `stream_strategy`, `action_timeout_sec` + rename of `runtime:` keys) | queued | Design §1. Parser change in `manifest_loader.hpp`, runtime wiring in `agent.cpp`. |
| `sandbox:` block: gates + `bind`/`files` live mounts | **done** | Full parse → SandboxPolicy; process symlinks + guest→host rewrite; docker `-v`; per-bind RO. `import.files` stays prompt-only. |
| `import.files` (prompt modules) | partial | Process injects `<module>` today; tag rename to `<imported_file>` + `import.folders` still queued. |
| "What's loaded" startup message — show counts of tools/feeds/relics/sub-agents/env/imports on agent start | queued | Design §7. Stderr line. Replace with status-bar indicator or `/loaded` slash command if user wants. |
| `auto_play_on_start: [tool calls]` | queued | Future feature for protocol alignment. Reduces bare-text rate. Separate from `import:`. |
