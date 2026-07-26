# Inkcell Migration — Living Plan

**Status:** ACTIVE (full migration path C)  
**Branch:** `feat/inkcell-agentshell`  
**Supersedes:** all docs under `docs/_archive/inkcell/`

## North star

inkcell is the terminal substrate. MK3 owns product scenes + domain.

```text
Agent thread  →  AgentBridge / eventfd wake  →  inkcell Engine / Scenes  →  Surface
```

Domain (protocol cards, sessions, manifests) stays in `agent-lib-cpp`.  
No agent/protocol types in `include/inkcell`.

## Flag semantics (locked)

| `--tui` / `MK3_TUI` | Path |
|---------------------|------|
| `experimental` (default) | Native inkcell App (`src/ui/*`, MainScene + AgentScene) |
| `inkcell` | **Alias of experimental** (honest naming) |
| `legacy` | `tui::ReplSession` oracle — ANSI 1:1 chat path |

Bare `cortex-mk3` (no `-p`, no `-m`) lands on the experimental control surface.

## Live product surface

| Piece | Path |
|-------|------|
| App wiring | `src/ui/app/mk3_tui_app.hpp` |
| Hub / dashboard | `src/ui/scenes/main_scene.hpp` |
| Chat | `src/ui/scenes/agent_scene.hpp` + `src/ui/chat/*` |
| Bridge | `src/ui/bridge/agent_bridge.hpp` |
| Models | `src/ui/model/*` |
| Cortex chrome | `src/ui/{components,theme,layout,gfx,assets}` |

## Legacy oracle (kept)

`src/tui/*` + `ReplSession` remain for `--tui legacy` and as visual/behavioral
oracle while experimental parity is finished. Not deleted.

## Archived (freeze, not delete)

- Orphan scenes → `src/ui/_archive/scenes/`
- Stale multi-plan docs → `docs/_archive/inkcell/`

## Remaining work (ordered)

1. **Hygiene** — gitignore root test bins; archive orphans ✅
2. **Flag honesty** — inkcell ≡ experimental; default experimental ✅
3. **Makefile smoke** — `ui-smoke` target; hooked into `all-tests` ✅
4. **Legacy wake_fd** — streaming already had eventfd; idle busy-loop fixed (50ms poll) ✅
5. **Experimental parity** — ask overlay / cancel / scroll / resume tests green ✅
   - Still open (non-blocking): pi ask_cards DAG (`condition`/`goto`/`optionsResolver`)
6. **Cutover** — legacy is opt-in only (`--tui legacy`) ✅

### Next sessions (optional)
- Top up opencode-go / free keys for real-token live-smoke
- Archive or keep test-only adapters (`timeline_view`, `agent_tree`) after test rewrite
- Shrink `src/tui/*` further only after operator approval

## Verify

```bash
make inkcell-lib
make cortex-mk3
make test-chat-scene test-ui-model
# optional: make test-ui-view test-ui-timeline test-perf
```

Live free-model smoke only after offline green.

## Non-goals

- Upstream Cortex hub chrome into inkcell
- Hard-delete of legacy TUI this session
- CMake / ncurses
