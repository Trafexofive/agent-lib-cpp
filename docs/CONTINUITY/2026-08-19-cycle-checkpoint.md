# Compaction checkpoint — 2026-08-19 evening

**Branch:** `feat/inkcell-agentshell`  
**Tip:** `67af8c4` fix(harness): never starve history of tool/subagent SoT  
**Binary:** `~/.local/bin/cortex-mk3` @ ~21:16  
**Repo:** `/home/mlamkadm/repos/active/agent-lib-cpp`  
**User binding:** grind issues; VC as you go; **LIVE TEST + dump-verify**; no bare chat questions (ask_cards only).

---

## Mission

Daily-driver cortex-mk3: truthful harness, usable chat instrument, no token starvation, no fake success.

---

## Verified this cycle (evidence)

### Live dump `ephemeral-1801446` (CORTEX_DEV_MODE + headless `--ephemeral --no-session`)
- Prompt: list docs → fs_read ARCHITECTURE → final
- **history.md:** `truncated="true"` count = **0**
- result bodies: list 257B full; fs_read **5599B full**
- Contains `Architecture Foundation` + `Dependency rules` (SoT present for next gen)
- iterations.md: no `truncated="true"`; max result body in iters ~17KB

### Prior dump autopsy `inkcell-1787169594733` (operator TUI)
- Double coder-worker scout (~65s+85s) for “light” ask
- Hollow agent `{}` then full body
- FALLBACK x-ai → minimax (curl write error)
- Child results **truncated at ~2KB** in history (pre-fix) — **cardinal sin**
- Report: `docs/AUDITS/REPORTS/2026-08-19-dump-inkcell-1787169594733.md`

### Live SoT regression (post-fix)
- Root cause: `history_.push_back(buildResultTag(id, result, true))` → 2KB cap
- Fix: history always `compact=false`; Full budget 512KiB safety only; Preview 4KB for UI-only
- File: `src/core/agent.cpp`, `src/core/agent_run_helpers.hpp`

---

## Shipped commits (recent tip chain)

| Commit | What |
|--------|------|
| `67af8c4` | Full SoT in history; hollow agent reject; canvas rename; dump reports |
| `de93e55` | 6-row daily-driver footer instrument |
| `0e38299` / `8395089` | Ctrl-O body views; empty-chat 400 fix install path |
| `10fbf0a` | iter≥2 always user message; retry empty-chat 400 |
| `a076844` | Footer rows 1–2 operator counters + ctx bar |
| `475623e` | Skip expired xai-auth; FALLBACK from→to |

---

## Code contracts (do not regress)

1. **History `<result>` = full tool/subagent output** (never Preview/2KB). UI may truncate paint only.  
2. **Hollow agent `{}`** → protocol_error, not execute child.  
3. **iter≥2** must include non-empty **user** message (opencode-go empty chat).  
4. **Empty-chat HTTP 400** → in-loop RETRY, not silent death.  
5. **FALLBACK** one STATUS via emitStatus + one harness System (no double STATUS push).  
6. **Chat body modes:** 0 stream · 1 compact · 2 **canvas** (Ctrl-O). Ctrl-Shift-O = truncate bodies.  
7. **Compact/canvas** consume `ChatSurfaceModel.timelineRows` (`activeRows` deque), not display-line reparse.  
8. **Footer** live 6 rows reserved; plain-English NOW + ctx bar + labeled counters.  
9. Subprocesses via `process::run` + wall clock.  
10. ask_cards for user questions — never bare chat Qs.

---

## Still open (ROI order)

| # | Item | Notes |
|---|------|------|
| 1 | Parser tests 2 fail | dup remap + invalid_json expectations drift |
| 2 | Double-delegate PE | one light scout → two workers; need PE + optional runtime gate |
| 3 | Prompt diet iter≥2 | iterations still ~100KB+ schemas every gen |
| 4 | Compact/canvas quality | wired to rows; still need live TUI polish / selection / edges |
| 5 | CURL write classify | partially done in FALLBACK path; confirm stall vs cancel |
| 6 | CLI headless | `--session` alone can open TUI; use `--ephemeral --no-session run -p` for dumps |
| 7 | Manifest seatbelts | dirty tree still has high max_iterations in some WIP agents |
| 8 | Dirty operator WIP | do not auto-commit: default USER.md, coder yml, libs/, harness.md WIP |
| 9 | Product app package | desktop entry / true app shell — started, not finished |
| 10 | wait/join adoption | tool exists; PE still teaches sleep |

---

## How to live-verify after resume

```bash
cd /home/mlamkadm/repos/active/agent-lib-cpp
export CORTEX_DEV_MODE=1
timeout 120 ./cortex-mk3 --ephemeral --no-session \
  --provider opencode-go --model deepseek-v4-flash --iterations 8 \
  run -p 'list docs max_entries=10; then fs_read a medium md; then final.'
# Then:
ls -td .cortex/dev/*/ | head -1
# history.md must show truncated="true" count 0 for normal scout-size bodies
rg 'truncated="true"' .cortex/dev/<latest>/history.md
```

TUI: install binary, Ctrl-O cycles stream/compact/canvas; footer 6-row live plate.

---

## Intentionally not committed

- `manifests/agents/default/USER.md`, `agents/agent.yml`, `harness/default.md` (operator WIP)
- `config/agents/coder/**`, `libs/`, `manifests/agents/stage/`
- `src/cli/commands.hpp`, `agent_tool_dispatch.cpp` wait tool if still dirty mixed

---

## Failure log (anti-loop)

- Do **not** re-add compact=true on history inject.  
- Do **not** claim dump verified without reading newest `.cortex/dev/*/history.md`.  
- Do **not** use `--session` alone for headless dump (opens hub TUI).  
- Compact/canvas without `timelineRows` wired = garbage; always set from `activeRows()`.  
- Starving LLM of results > “saving tokens” — user hard rule.

---

*GODSPEED — next agent: start at open #1–3, live-test every slice.*
