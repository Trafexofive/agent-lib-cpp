# MK3 Live Tickets

Open, in-progress, and recently-resolved tickets for the MK3 codebase. Anything
historical and not part of the live backlog lives in
[`docs/archive/`](./archive/).

## Authoritative source

The current open audit findings live as an artifact and are kept in sync as
slices ship:

- `agent-lib-cpp-poc-stub-audit-2026-06-20` (artifact `art-mqmo4hl5-w3npoz`)

When the audit changes, this file should be updated to reflect the new
priorities — not the other way around.

## Live open tickets

The list below carries only items the user has explicitly prioritized. Each
slice gets its own commit so a bisect stays clean.

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
