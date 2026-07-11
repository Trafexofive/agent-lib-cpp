# Inkcell Legacy TUI Port Plan — 1:1 View Parity

**Date:** 2026-07-11  
**Branch:** `feat/inkcell-agentshell`  
**Status:** AUTHORITATIVE BUILD PLAN  
**Goal:** Make `--tui inkcell` look and behave like the **current legacy REPL TUI**, not a redesigned sbtui product.  
**Non-goal (this plan):** Welcome-only shell, Dashboard/Inspector pages, decorative redesign, “unreasonably good” polish. That comes **after** parity.

---

## 0. Thesis (locked)

The scuffed work failed because it **redesigned** before it **ported**.

The legacy TUI is already a working product surface:

```text
historyLines (ANSI rows)
  + TuiRenderer.renderTranscript(events, response)
  + SessionView bottom-anchored viewport + dirty-row diff
  + status bar (spinner / phase / provider/model / session)
  + prompt line (▸ cursor inverse)
  + tui::Input (history, slash complete, scroll, esc cancel)
  + agent worker thread + stream snapshot mutex
  + ask_tool dialog overlay
```

**1:1 port means:** that pipeline remains the visual/interaction source of truth.  
**inkcell means:** host the event loop (poll, wake, resize, alt-screen lifecycle) — not invent new chrome.

If a change would make the screen look different from `legacy` at the same terminal size with the same events, it is **out of scope**.

---

## 1. Hard gaps filled (decisions — not left to the builder)

### Gap A — “Port to inkcell” vs “look like legacy”

**Decision: Hybrid host, legacy paint.**

| Layer | Owner after port |
|-------|------------------|
| Protocol row generation | **keep** `src/tui/renderer.hpp` + `components/protocol.hpp` + `markdown.hpp` |
| Viewport / scroll / dirty rows | **keep** `src/tui/session_view.hpp` |
| Status + prompt strings | **keep** `src/tui/status_prompt.hpp` |
| Input editing / history / slash | **keep** `src/tui/input.hpp` + `history.hpp` + `keys.hpp` |
| ask_tool dialogs | **keep** `src/tui/dialog.hpp` |
| Frame pacing / spinner index | **keep** `src/tui/frame_clock.hpp` |
| Event loop + agent wake | **inkcell Engine** (or Engine-shaped host) |
| Agent stream bridge | **reuse** bridge idea, but feed **legacy snapshot fields**, not custom TimelineRow UI |

**Rejected:** Re-drawing transcript through inkcell `Surface` cells first. That forces ANSI→cell conversion, loses exact escapes, and guarantees visual drift.  
**Rejected:** New Welcome/Dashboard multi-page app as the inkcell path. Product later; parity first.

### Gap B — Does inkcell Surface participate in v1?

**Decision: No for body/status/prompt paint.**

v1 inkcell path writes the **same ANSI frame strings** SessionView already produces (`render` / `renderFull`) to stdout, exactly like `main.cpp` today.

inkcell is used for:

1. optional alt-screen enter/exit consistency via its Terminal, **or** keep legacy `\033[?1049h` path if Terminal fights Input raw mode — **must pick one owner of termios** (see Gap C).
2. `wake_fd` + `input_poll_ms` so agent thread can force frames without busy-wait redesign.
3. resize observation if Engine already has it; else keep `SIGWINCH` + `g_resized` as today.

**Later (post-parity):** optionally paint SessionView frame rows into Surface for structured testing. Not blocking.

### Gap C — Two raw-mode owners (Input vs inkcell Terminal)

**Hard conflict:** `tui::Input::start()` calls `cfmakeraw` / `tcsetattr`. inkcell `Terminal` also owns raw mode + alt-screen.

**Decision: Single owner — `tui::Input` keeps termios in v1.**

- inkcell Engine must run in a mode that **does not** re-raw the terminal, **or**
- do not call `Engine::run()` as a full terminal owner; instead implement a thin `LegacyReplHost` loop that:
  - uses `eventfd` wake like AgentBridge
  - uses `Input::poll()` as today
  - uses SessionView ANSI output as today
  - is **invoked** from `--tui inkcell` entry

**Name it honestly:** v1 “inkcell path” = **extracted legacy REPL + inkcell-grade wake/bridge host**.  
Full Engine Scene graph is **v2**, after pixel parity is proven.

If the builder tries to force TextArea + Scene for the prompt line in v1, **reject**. Prompt must stay `StatusPromptRenderer::promptLine(Input)`.

### Gap D — Where does the god-loop live?

Today ~1k+ lines of TUI glue lives in `main.cpp` (`cmdRun` interactive branch).

**Decision: Extract, don’t rewrite.**

```text
src/tui/repl_session.hpp   # (or .cpp if too large)
  class ReplSession {
    int run(ReplConfig, Agent&, Cli bits...);
  };
```

`main.cpp` becomes:

```text
if (cli.tuiMode == "legacy" || cli.tuiMode == "inkcell")
    return ReplSession(...).run(...);
```

**v1:** `legacy` and `inkcell` call the **same** `ReplSession`.  
Difference allowed only if documented:

| Flag | Behavior v1 |
|------|-------------|
| `--tui legacy` | `ReplSession` (extracted from current main) |
| `--tui inkcell` | **same** `ReplSession` |

Yes — that means v1 inkcell **aliases** legacy after extraction.  
That is intentional. The “port” first step is **structural isolation + zero visual change**. Claiming inkcell while changing chrome is what produced scuff.

**v1.1 (still parity):** ReplSession internal loop gains AgentBridge wake_fd instead of pure usleep polling — **no visual change**.

**v2:** Optional inkcell Engine shell around ReplSession paint functions for tests/snapshot — still same rows.

### Gap E — Current `src/ui/**` scuffed shell

**Decision: Quarantine, do not delete yet (no unapproved deletion).**

```text
src/ui/   →  treat as experimental / non-default
```

- Default remains behavioral legacy via ReplSession.
- Do not invest more in Welcome/Dashboard/AgentScene chrome until parity lands.
- Optional: `--tui experimental` if we keep the scuffed shell for later redesign work.
- Do **not** make experimental the meaning of `--tui inkcell`.

Rename semantics:

| Flag | Meaning |
|------|---------|
| `legacy` | ReplSession (current product TUI) |
| `inkcell` | ReplSession hosted under inkcell wake/loop (v1 = same code path) |
| `experimental` | old multi-scene redesign (optional) |

### Gap F — Sub-agent history drill-down

**Out of scope for 1:1 port.**  
Legacy doesn’t have it. Add only after parity checklist is green. Spec it separately; do not block port.

### Gap G — One-shot `-p` path

Legacy one-shot currently prints markdown to stdout (non-alt-screen) unless REPL.  
Interactive REPL is the parity target.

**Decision:**

- `-p` without `--repl`: keep non-TUI / simple stdout path (legacy behavior).
- `-p --repl` or bare interactive: ReplSession alt-screen.
- Do not force inkcell multi-scene one-shot UI.

### Gap H — Snapshot / CI

Legacy has `--tui-debug-dump` and frame capture hooks.

**Decision:** Preserve dump paths. Add:

```bash
./cortex-mk3 --tui inkcell ...  # must produce identical dump shape to legacy
```

Parity test (manual or scripted):

1. Same prompt, same provider mock/fixture if available.
2. Dump render lines under both flags.
3. Diff normalized (strip spinner frame glyph / elapsed timer fields).

---

## 2. Legacy surface inventory (must preserve)

### 2.1 Screen geometry

```text
row 1 .. H-2 : bottom-anchored transcript viewport (history + live renderer)
row H-1      : status bar
row H        : prompt line ▸ ...
```

- Scroll offset: half-page via Input scroll hooks.
- History is sticky archived ANSI; live turn is renderer lines; on turn complete, renderer archive merges into history and live clears.

### 2.2 Modules (do not reimplement)

| File | Role |
|------|------|
| `renderer.hpp` | FULL/SEMI/RAW transcript |
| `components/protocol.hpp` | action/result cards |
| `components/markdown.hpp` | response markdown |
| `session_view.hpp` | viewport + dirty diff |
| `status_prompt.hpp` | status + prompt |
| `input.hpp` | line edit, history, keys |
| `history.hpp` | `~/.mk3_history` |
| `slash_commands.hpp` | `/help` etc. |
| `dialog.hpp` | ask_tool |
| `frame_clock.hpp` | pacing + spinner |

### 2.3 Concurrency model (must preserve)

```text
UI thread: Input.poll → applyStreamSnapshot → renderScreen
Agent thread: agent.prompt(onToken) → snap* under mutex → agentDone
ask_tool: agent blocks on CV; UI drives DialogState
```

Do not move provider calls onto UI thread.  
Do not render from agent thread.

### 2.4 Visual details that define “same TUI”

These are easy to lose — treat as acceptance tests:

1. User echo: gray `48;2;45;45;50` block, bold `▸ ` prompt text, top/bottom pad rows.
2. Action/result cards from ProtocolView (not `◆ title` plain lines).
3. Status spinner glyphs `⠋⠙…` while streaming + phase string.
4. Prompt inverse-video cursor (`\033[7m`).
5. Dim italic history system lines for slash output.
6. Bottom-anchored scroll (new content appears above status, not top-down app chrome).
7. Mode cycle FULL/SEMI/RAW if legacy keybind exists — preserve.
8. `/dump-render`, `/clear`, session slash commands behavior.

---

## 3. Target architecture after port

```text
main.cpp
  parse CLI → build Agent → 
    ReplSession::run(config)     # extracted from current interactive loop

src/tui/repl_session.hpp
  owns: Input, SessionView, TuiRenderer, FrameClock, stream mutex, ask_tool bridge
  paints: SessionView ANSI frames
  host:  wake_fd optional (v1.1)

src/ui/**  (experimental only)
  frozen / non-default
```

Dependency rule:

```text
ReplSession → src/tui/* → agent
inkcell (optional host helpers) → no dependency on ReplSession visuals
experimental src/ui → may depend on inkcell; must not be default
```

---

## 4. Execution phases (for the building model)

### Phase 0 — Extract ReplSession (no behavior change)

**Work:**

1. Move interactive loop body from `main.cpp` into `src/tui/repl_session.hpp` (header-only OK if matches project style; split `.cpp` if compile times hurt).
2. Define `struct ReplSessionConfig` with only fields the loop needs (provider/model names, session id, ephemeral, toolAnsi, tuiDebugDumpPath, sessionName, etc.).
3. `cmdRun` interactive branch becomes ~20 lines: construct config, `return ReplSession(cfg).run(agent);`
4. One-shot non-repl path stays in `main.cpp` (or thin helper) unchanged.

**Verify:**

```bash
make cortex-mk3
# interactive manual: same keys, same look as before extraction
./cortex-mk3 --tui legacy --no-session   # if flag exists; else default interactive
```

**Exit:** visual identical to pre-extract; `main.cpp` no longer contains renderScreen lambda forest.

### Phase 1 — Flag honesty

**Work:**

1. `--tui legacy` → ReplSession  
2. `--tui inkcell` → ReplSession (same)  
3. Optional `--tui experimental` → current `src/ui` multi-scene (if kept)  
4. Help text states clearly: inkcell is host path; experimental is redesign sandbox.

**Verify:** both legacy and inkcell dumps match (normalized).

### Phase 2 — Wake host (still 1:1 pixels)

**Work:**

1. Add `eventfd` wake to ReplSession (can reuse `src/ui/bridge/agent_bridge.hpp` **only as queue/wake**, not TimelineRow UI).
2. Agent onToken publishes snapshot + wake.
3. UI loop `select()` on stdin + wake_fd (or poll wake then Input::poll).
4. Remove reliance on lucky usleep for first-byte snappiness **without** changing frame contents.

**Verify:** spinner still animates on heartbeat; no double-raw-mode; cancel still works.

### Phase 3 — Optional inkcell Terminal lifecycle

**Work:** only if Phase 2 stable.

1. Evaluate whether inkcell Terminal alt-screen can wrap ReplSession without fighting Input.
2. If conflict: keep legacy alt-screen sequences; document.
3. If compatible: Terminal enter/exit only; paint path unchanged.

### Phase 4 — Parity harness

**Work:**

1. Script or test that feeds fixture protocol events into TuiRenderer + SessionView, dumps rows.
2. Golden files under `tests/tui/` (extend existing `tests/tui/render_test.cpp` / `renderer_test.cpp`).
3. Spinner/time fields normalized away for golden diff.

### Phase 5 — Only then: redesign / history drill / welcome

**Blocked until Phase 4 green.**

Then, and only then:

- sub-agent history drill-down
- sbtui DESIGN.md visual language migration
- TextArea multiline composer
- Scene graph pages

---

## 5. File-level change map

### Create

```text
src/tui/repl_session.hpp          # extracted interactive loop
docs/INKCELL_LEGACY_PORT_PLAN.md  # this file
tests/tui/parity_notes.md         # optional checklist results
```

### Modify

```text
main.cpp                 # thin dispatch into ReplSession
Makefile                 # only if new .cpp
```

### Freeze / ignore for port

```text
src/ui/app/*
src/ui/scenes/*
src/ui/views/*
src/ui/model/*           # except if AgentBridge wake is reused
```

### Reuse as-is

```text
src/tui/renderer.hpp
src/tui/session_view.hpp
src/tui/status_prompt.hpp
src/tui/input.hpp
src/tui/dialog.hpp
src/tui/frame_clock.hpp
src/tui/slash_commands.hpp
src/tui/components/*
```

---

## 6. ReplSession API sketch (concrete)

```cpp
namespace cortex::mk3::tui {

struct ReplSessionConfig {
    std::string provider;
    std::string model;
    std::string sessionId;
    std::string sessionName;
    std::string tuiDebugDumpPath;
    bool ephemeral = false;
    bool toolAnsi = true;
    bool persistSession = true;
    // ask_tool / dump hooks as needed
};

class ReplSession {
public:
    explicit ReplSession(ReplSessionConfig cfg);
    // Returns process exit code.
    int run(Agent& agent);
};

}  // namespace
```

Internal methods (private), mirroring current lambdas:

- `renderScreen(bypassPacing, fullRedraw)`
- `applyStreamSnapshot()`
- `statusState()` / `statusBarText` / `promptLineText`
- `pushTuiLine` / slash command handler
- `runTurn(promptText)` — starts agent thread, polls until done
- `handleResize()`

**Rule:** move code first; clean second. No “while extracting, also fix UI.”

---

## 7. AgentBridge role (clarified)

| Use | v1 port |
|-----|---------|
| eventfd wake | yes (Phase 2) |
| UiEvent TimelineRow model | **no** for default paint |
| experimental scenes | only `--tui experimental` |

If bridge is reused, add a **snapshot publisher** path:

```text
onToken → mutex snapEvents/snapResponse → wake
UI → applyStreamSnapshot → renderer.renderTranscript → SessionView
```

That is the legacy model. Do not convert events into ShellModel rows for default path.

---

## 8. Definition of done (1:1 parity)

### Must pass

- [ ] Interactive session under `--tui inkcell` is visually indistinguishable from pre-port legacy at same `termW x termH` for: idle prompt, user echo block, streaming spinner, action cards, result cards, final response markdown, slash `/help` lines.
- [ ] Esc cancels in-flight generation.
- [ ] ask_tool dialog still blocks agent and accepts input.
- [ ] Scroll up/down preserves bottom-anchored behavior.
- [ ] Resize does not corrupt alt-screen.
- [ ] `~/.mk3_history` still loads/saves.
- [ ] Session resume still seeds `historyLines` / rendered history as before.
- [ ] `main.cpp` interactive loop extracted (no duplicate live loop left behind).
- [ ] Experimental multi-scene UI is not default.

### Explicitly not required for done

- [ ] Welcome page
- [ ] Sub-agent drill-down
- [ ] TextArea multiline
- [ ] DESIGN.md sbtui full compliance
- [ ] Painting via inkcell Surface cells

---

## 9. Builder anti-patterns (reject in review)

1. Redesigning prompt to TextArea “because inkcell has it.”
2. Replacing ProtocolView cards with plain `◆ title` lines.
3. Top-down app chrome (title bars, side nav) on the default path.
4. Using Engine Scenes as the default interactive path before ReplSession extraction.
5. Mixing experimental ShellModel into ReplSession.
6. “While we’re here” sub-agent navigation.
7. Claiming parity without side-by-side dump or live comparison.

---

## 10. Recommended first PR sequence (git-solid)

1. **`refactor(tui): extract ReplSession from main.cpp`**  
   - files: `src/tui/repl_session.hpp`, `main.cpp`  
   - zero intentional behavior change

2. **`chore(tui): map --tui inkcell to ReplSession; quarantine experimental UI`**  
   - help text + flag routing

3. **`perf(tui): wake_fd stream host inside ReplSession`**  
   - still 1:1 pixels

4. **`test(tui): golden transcript rows for renderer+session_view`**

Do not squash redesign into PR 1.

---

## 11. Handoff prompt for building model (copy/paste)

```text
Implement docs/INKCELL_LEGACY_PORT_PLAN.md Phase 0–1 only.

Rules:
- 1:1 visual parity with current legacy REPL TUI.
- Extract main.cpp interactive loop into src/tui/repl_session.hpp.
- --tui legacy and --tui inkcell both run ReplSession.
- Do NOT redesign chrome, TextArea, Welcome, Dashboard, or sub-agent drill.
- Do NOT delete src/ui; quarantine as experimental if needed.
- Verify: make cortex-mk3; smoke interactive if possible; no behavior change intended.

Hard decisions already made in the plan — follow them; do not reopen architecture.
```

---

## 12. Why this plan is the serious one

The product TUI already exists in `src/tui/*` + `main.cpp`.  
inkcell is a substrate.  
Parity is an **extraction + hosting** problem, not a greenfield UI problem.

Everything scuffed so far inverted that order. This plan inverts it back.

---

*After Phase 4 is green, open a separate plan for history drill-down and sbtui visual migration. Not before.*
