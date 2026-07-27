# inkcell ↔ Cortex MK3 integration

**Living dual-repo contract.** First product (agent-lib) drives inkcell maturity; inkcell primitives must be dogfooded, not forked forever in product headers.

Full session ledger (post-compaction): artifact `inkcell-agentlib-dual-repo-ledger`.

## Repos

| | agent-lib-cpp | inkcell | markdown-buddy |
|--|---------------|---------|----------------|
| Path | this repo | `../inkcell` (`INKCELL_ROOT`) | `../markdown-buddy` |
| Branch (typical) | `feat/inkcell-agentshell` | `main` | `main` |
| Role | Agent product (Cortex) | App-agnostic TUI engine | **Ship proof** — glow×50 reader |

`inkcell/examples/markdown-buddy` → symlink to `../../markdown-buddy`.  
POC demos live under `inkcell/examples/_archive/`.

## Doctrine

1. **Domain never enters inkcell** (agent, session, protocol, providers).
2. **Dogfood or upstream** — new chrome/focus/commands either use inkcell APIs or become inkcell PRs.
3. **One focus model** — no additional `bool *Focus` flags.
4. **Actions over strings** — no new `pendingRoute = "…"`.
5. **Cancel ≠ quit** — product cancel token / `g_running`; engine quit is separate.
6. **Legacy TUI is oracle** (`--tui legacy`) — no new features.
7. **Extract-as-we-go** — SessionController-style seams over monolith growth.

## What product uses today

| inkcell | Cortex usage |
|---------|----------------|
| Engine + App + Scene | Yes — tick 33ms, wake_fd, render_to tests |
| Surface / Renderer | Yes — all draw |
| TextAreaState / ScrollViewState | Partial — state yes, custom paint |
| ui::Shell / Region / Theme roles | **Underused** — hand-rolled chrome + `cortex_theme.hpp` |
| CommandRegistry / KeyHints / StatusBar | **Underused** |
| FocusManager | **Unused** — multi ad-hoc focus flags |
| Widgets (Dialog, List, …) | Mostly reinvented |

## Integration gaps (priority)

### inkcell should grow (from product pain)

1. Focus tree / modal stack  
2. CommandRegistry → help overlay  
3. StatusBar slots (metrics / pills)  
4. COOKBOOK: wake_fd + coalesce-on-tick recipe (Cortex pattern)  
5. ScrollView BYO-paint / span policy (or document BYO)  
6. Typed actions / routes  
7. Layout debug overlay  

### agent-lib should fix

1. Commit remaining S-pack + sanitize if still dirty (explicit paths)  
2. Unify `runInkcell*` into one runtime RAII (tick/flush/atexit)  
3. Peel ShellModel: EventReducer, TimelineStore, Projection, ViewModel  
4. Collapse focus + routes  
5. QoL packs under `docs/tui-qol/` (feel / composer / nested) — optional detour  

## Related docs

- `docs/INKCELL_MIGRATION.md` — cutover / archive (mostly done)  
- `docs/tui-qol/*` — operator QoL packs  
- `AUDITS/REPORTS/2026-07-26-*.md` — session / perf / modularity  
- inkcell `docs/DESIGN_PRINCIPLES.md`, `COOKBOOK.md`, `dev-qol-backlog.md`  

## Verify

```bash
# inkcell
make -C ../inkcell lib
# agent-lib
make inkcell-lib cortex-mk3 test-chat-scene test-ui-model test-session-controller test-perf -j$(nproc)
```

## Uncommitted reminder (session snapshot)

If working tree still has S-pack/sanitize, stage **only**:

```
main.cpp
src/session/controller.hpp
src/testing/session_controller_test.cpp
src/testing/ui_model_test.cpp
src/ui/app/mk3_tui_app.hpp
src/ui/model/dashboard_controller.hpp
src/ui/model/inkcell_app_model.hpp
src/ui/scenes/main_scene.hpp
docs/tui-qol/
docs/INKCELL_INTEGRATION.md
```

Never `git add -A` (unrelated config deletes exist on branch).
