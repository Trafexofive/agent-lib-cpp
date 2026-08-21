# Continuity — 2026-08-20 pre-compaction

Repo: `feat/inkcell-agentshell` → `origin/master`  
Binary: `~/.local/bin/cortex-mk3` — **restart after install**.

## Tip (this session)

| SHA | Slice |
|-----|--------|
| `54794cb` | sibling AGENT cards paint before first join; footer **fixed 5 rows** |
| `8bb989b` | ask_tool one contract (`normalizeAskParams` + result keys) |
| `6bf5c31` | one BARE_TEXT/NONFINAL per turn; thought-streak machine deleted |
| `abfcf59` | never mint `<response>` from thought; no thought-promote-at-cap |
| `d56487b` | bare → non-final continue (superseded: untagged stays thought) |
| pending | AGENT header `mode`/`eph`/`tNs` + child well engine/iter |

## Operator live dumps (keep)

- Dual ping export: session `inkcell-1787333584342` — two `coder-worker` present; UI only showed `#ping-a` until join. **Root:** SYNC `doExecute` blocked parser feed. **Fix:** agent actions spawn `std::async`; generation still `waitForActions`. Same-name child serialized via `subAgentRunMus_`.
- Coder ping: `.cortex/dev/ephemeral-135985` — iter1 action, 2–4 empty thinking, iter5 final.

## Binding facts (do not regress)

- Chat body: **stream ↔ compact only**. Chat canvas **removed**. Workflow canvas is a different page.
- Footer: **always 5 rows**. No sin-pulse, no live height jump.
- Session identity: persist `metadata.manifest_path`; never overwrite a real disk agent from default/placeholder.
- `runLoop` is still **~1089 lines** (`agent.cpp:272–1361`). Not split. Next cut: generate-error **or** dispatch — pick one, extract a function, same behavior.
- Untagged TEXT is **Thought** (parser). Incomplete gen: one structured `<harness code=BARE_TEXT|NONFINAL>`. Cap does not promote thoughts.
- `thinking_level` always in `<reasoning_policy>`.
- ask_tool: `src/tools/ask_protocol.hpp` is the wire. TUI + stdin + dispatch share it.

## Intentionally not touched

- Dirty untracked: `config/agents/*`, `libs/`, `sessions/`, `state/`, operator `manifests/{PLANNED,harness,persona}`.
- Mass `src/ui` rewrite, curl_multi, settings SPA two-pane, Home/Sessions overhaul (plan only: `docs/tui-qol/08-home-sessions-bodyviews-overhaul.md`).
- Loop decoupling (operator: “runLoop is still 1000 lines” — acknowledged, not started as a pile-move).

## Open (ROI order)

1. **runLoop split** — extract generate-error or dispatch, not a file rename.
2. Empty thinking gens still **count iterations** (BARE_TEXT once, then quiet). Optional later: don’t increment work slot on 0-byte protocol.
3. Home/Sessions launchpad (plan exists, zero UI).
4. Footer craft still operator-sensitive (5 rows given; still not “museum”).
5. Remaining harness slogans: TIMEOUT/CANCEL/RETRY/FALLBACK/LIMIT/STEER (BARE_TEXT/NONFINAL have structure).
6. Settings SPA remainder (persist+labels shipped).
7. Prompt-building tickets + indent (authorized, not executed).

## Verify last known

- `test-ui-model` `test-chat-scene` `test-parser` `test-completion-policy` `test-ask-cards` green at last install.
- Restart TUI after this commit to see AGENT `sync/async eph` chips + child well engine line.
