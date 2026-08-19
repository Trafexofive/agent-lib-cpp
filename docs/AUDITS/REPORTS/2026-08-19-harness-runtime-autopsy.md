# Harness / runtime autopsy — 2026-08-19

Live dumps: `inkcell-1787159608182`, `inkcell-1787164550964`

## What burned compute

1. **Sub-agent `max_iterations: 1800`** on default child + coder 400 — child can thrash forever.
2. **Agent `mode=async` + `waitForActions(30s)`** — parent TIMED OUT while child ran 249s; operator saw TIMEOUT; child kept spending tokens until Ctrl-C.
3. **Hollow `tree {}` then full body** — empty cwd dumps + id replay confusion.
4. **Result summary dropped `tree` field** — UI showed `tree` 4B while history had full JSON.
5. **Timeout path** only `emitStatus(System: …)` — no structured `<harness>` for the model, STATUS easy to miss vs RESPONSE.

## Prompt assembly (where things live)

| Layer | Source | Role |
|-------|--------|------|
| `<harness><protocol>` | `harnessText_` from harness.md (ctor cache) | CANON / protocol law |
| `<persona>` / system / USER | manifest context paths | behavior |
| tools / sub_agents cards | runtime capabilities + imports | action surface |
| history_ | User/Agent/System + runtime injects | multi-turn |
| runtime `<harness kind code>` | **emitHarness** (new) | mid-run limits/timeouts/cancel |

`buildSystemPrompt` always wraps static harness.md. **Dynamic** limits must be **history System lines** with `<harness …>` so the *next* generation sees them without rebuilding the whole system block.

## Chat paint

- `protocolEvents_ STATUS` → protocol diff → timeline Status row → `⚠ LIMIT` / `⏱ TIMEOUT` / `⏹ CANCEL`
- `finishTurn` still closes with RESPONSE so the turn has a terminal block; STATUS is the operator-facing reason.

## Fixes in this pass

- Force agent ASYNC → SYNC (join)
- Join floor 120s; child iter clamp ≤48 on delegate
- default-sub-agent yml 48
- empty action body reject; tree path required; summary reads `tree`
- `wait`/`join` tool; emitHarness dual-channel

## Still open

- Full prompt_building audit (schemas on/off, examples bloat in iterations.md)
- Footer instrument finish + body chroma finish (WIP)
- Graph alt-view
