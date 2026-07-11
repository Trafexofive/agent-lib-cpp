# Inkcell Full Migration Plan — MK3 Unreasonably Good TUI

**Date:** 2026-07-11  
**Status:** PLAN / STARTING POINT  
**Artifact:** `art-mrflxfd4-gupoyt` (`inkcell-full-migration-plan-2026-07-11`)  
**Execution model:** plan here → implement on gpt-5.5 (or equivalent) coding session

This is the authoritative migration plan for moving Cortex-Prime MK3 off the
hand-rolled `src/tui/*` + `main.cpp` REPL glue onto **inkcell** as the terminal
substrate.

---

## 0. North star

Build a TUI that feels like a **local agent IDE in the terminal**, not a chat wrapper:

- Instant, flicker-free, resize-safe
- Protocol-native (thought / action / result / response as first-class visual objects)
- Multimodal operator surface: chat, inspect, orchestrate, debug, manage manifests
- Unreasonably good QOL: keyboard-first, discoverable, recoverable, beautiful without noise
- Clean separation: **inkcell = engine**, **MK3 = product scenes + domain**

### Non-goals

- Do **not** put agent protocol / tool schemas / LLM providers into inkcell
- Do **not** rewrite the agent loop inside the TUI
- Do **not** depend on ncurses / CMake / GUI toolkits
- Do **not** freeze on visual polish before the agent bridge is solid

---

## 1. Architecture (locked)

```text
┌─────────────────────────────────────────────────────────────┐
│  MK3 product (agent-lib-cpp)                                │
│  scenes: AgentShell, ManifestManager, SessionPicker, ...    │
│  domain widgets: ProtocolTimeline, ActionCard, ResultCard   │
│  bridges: AgentBridge (thread-safe event bus)               │
└──────────────────────────▲──────────────────────────────────┘
                           │ Actions / Events / Snapshots
┌──────────────────────────┴──────────────────────────────────┐
│  inkcell (engine)                                           │
│  Engine · Scene · Surface · Renderer · KeyMap · Theme       │
│  Shell · widgets (Input/TextArea/ScrollView/List/Dialog…)   │
│  wake_fd · input_poll · post_action · diff frames           │
└─────────────────────────────────────────────────────────────┘
```

| Owns | inkcell | MK3 |
|------|---------|-----|
| Terminal raw mode / alt screen | ✅ | ❌ |
| Diff renderer / surface | ✅ | ❌ |
| Generic widgets | ✅ | ❌ |
| Key → Action mapping substrate | ✅ | product maps |
| Agent thread / protocol parse | ❌ | ✅ |
| Protocol visualization | ❌ | ✅ |
| Manifest catalog UX | ❌ | ✅ |
| Slash commands / session I/O | ❌ | ✅ |

### Runtime model

```text
Agent thread                     UI thread (inkcell Engine)
─────────────                    ─────────────────────────
prompt() / stream tokens    →    eventfd wake
parse XML tags              →    append ProtocolEvent snapshot
dispatch tools              →    ActionCard / ResultCard updates
ask_tool pending            →    Dialog scene / modal overlay
final response              →    transcript commit + idle prompt
```

**Hard rule:** UI never blocks on LLM/network. Agent never draws ANSI.

---

## 2. Current inventory

### MK3 (`src/tui` ~4.4k LOC + fat `main.cpp` loop)

| Module | Role | Migration fate |
|--------|------|----------------|
| `terminal.hpp` / `surface.hpp` / `width.hpp` | low-level ANSI | **replace** with inkcell |
| `keys.hpp` / `input.hpp` / `history.hpp` | raw input + history | **replace** (+ keep history store) |
| `renderer.hpp` | FULL/SEMI/RAW transcript | **port domain** → ProtocolTimeline |
| `components/protocol.hpp` | Action/Result cards | **keep domain**, redraw on inkcell |
| `components/markdown.hpp` | markdown → lines | **keep** |
| `session_view.hpp` | viewport compositor | **replace** Shell + ScrollView |
| `dialog.hpp` | ask_tool cards | **map** to inkcell dialogs |
| `slash_commands.hpp` | slash registry | **keep** |
| `status_prompt.hpp` | status bar | **map** StatusBar |
| `manifest_manager.hpp` | two-pane manager | **rebuild** as scene |
| `main.cpp` REPL glue | god-loop | **extract** to scenes + bridge |

### inkcell foundation

| Ready | Baseline added | Still needed for unreasonable |
|-------|----------------|-------------------------------|
| Engine/Scene/App | TextArea | hardened multiline (wrap/paste/select) |
| Surface + diff | ScrollView | virtualized huge lists |
| KeyMap/Actions | wake_fd / input_poll | dual-thread bridge examples |
| Shell/Region | wcwidth basic | focus graph, modal stack, split panes |
| Many widgets | | toasts, selection, optional mouse |

---

## 3. Target product surfaces

1. **AgentShell** — transcript + composer + status (default)
2. **ManifestManager** — global `manifests/agents` + ownership tree
3. **SessionBrowser** — list/search/resume/fork/export
4. **ProviderPicker** — provider → model
5. **Inspector** — raw stream, events, dumps
6. **CommandPalette** — all actions searchable

### Feature pillars

- **Protocol UI:** FULL/SEMI/RAW; action/result cards; collapse thoughts; stick-bottom scroll
- **Composer QOL:** multiline, history, slash completion, paste-safe, abort grades
- **Sessions:** resume, name, fork, switcher
- **Operator:** manifest manager, surface panel, ask_tool modals, harness size switch
- **Polish:** themes, key hints, resize-safe, no flicker, latency HUD

---

## 4. Migration strategy

**Strangler fig**, not big bang.

```text
--tui inkcell   # new path
--tui legacy    # current main.cpp loop (default until cutover)
```

Env: `MK3_TUI=inkcell|legacy`

Dependency:

```text
agent-lib-cpp  →  inkcell (sibling ../inkcell)
inkcell        ↛  agent-lib-cpp
```

---

## 5. Phases

| Phase | Goal | Exit |
|-------|------|------|
| **0 Foundations** | link inkcell, empty shell scene, `--tui` flag | **shipped** — snapshot smoke green |
| **1 Agent bridge** | eventfd + UiEvent queue + stream publish | **one-shot shipped** — `--tui inkcell -p` streams through AgentBridge into inkcell snapshot/live shell |
| **2 AgentShell MVP** | REPL parity | real interactive work possible |
| **3 Protocol widgets** | structured timeline, not dumb lines | FULL mode feels native |
| **4 Operator scenes** | -m / -r / provider pickers on inkcell | all pickers are scenes |
| **5 QOL / power** | paste, palette, drawers, HUD, themes | delight |
| **6 Cutover** | default inkcell; delete legacy | docs + fixtures green |

### Sprint order for coding sessions (start here)

**A Wire & skeleton** → **B Bridge** → **C Composer REPL** → **D Protocol cards** → **E Port pickers**

Do not theme-festival or pet-mascot before Sprint C works.

---

## 6. Target package layout

```text
src/ui/
  model/      inkcell_app_model.hpp          # data/view state only
  theme/      cortex_theme.hpp               # semantic style tokens only
  layout/     sbtui_layout.hpp               # generic layout/drawing helpers
  views/      shell_views.hpp                # render helpers, no key handling
  scenes/     agent/dashboard/inspector/help # controllers + scene-specific routing
  bridge/     agent_bridge.hpp, ui_event.hpp # thread-safe Agent -> UI conduit
  app/        mk3_tui_app.hpp                # assembly + worker lifecycle
src/tui/      LEGACY — freeze, then delete after cutover
```

`main.cpp` becomes CLI + `runLegacyTui()` / `runInkcellTui()`.

---

## 7. Agent bridge sketch

Implemented baseline:

```text
src/ui/bridge/ui_event.hpp
src/ui/bridge/agent_bridge.hpp
```

```cpp
enum class UiEventKind {
  Token, Protocol, Status, AskDialog, AskDialogResult, TurnDone, Error, Log
};

class AgentBridge {
  int wakeFd() const;
  void publish(UiEvent);
  std::vector<UiEvent> drain();
  UiSnapshot snapshot() const;
};
```

- Agent thread: publish only  
- UI thread: drain on wake/tick, update, draw  
- Never call provider from UI thread

---

## 8. inkcell upstreamables

| Pri | Item |
|-----|------|
| P0 | wake_fd + input_poll (baseline done) |
| P0 | ScrollView stick-bottom (baseline done) |
| P0 | TextArea multiline (baseline; harden) |
| P0 | Focus / modal stack |
| P1 | Virtualized list, SplitPane, bracketed paste, selection |
| P2 | Unicode width tables, reduced-motion, optional mouse |

Domain-agnostic → inkcell. Knows `ProtocolEvent` → MK3.

---

## 9. Defaults (unless overridden)

| Decision | Default |
|----------|---------|
| Default TUI until parity | `legacy` |
| Send key | Ctrl-Enter send, Enter newline |
| Navigation | modeless |
| inkcell path | sibling `../inkcell` |
| Protocol mode | FULL |
| Thoughts | collapsed by default (`t` toggles) |
| Mouse | off until keyboard is excellent |

---

## 10. Success criteria

**MVP cutover-ready:** interactive REPL, streaming, tool cards, ask_tool, resize-safe, manifest manager, long transcript usable.

**Unreasonable:** faster than chat UIs; protocol is the visual language; palette/hints make power discoverable; operators never go back to raw logs.

---

## 11. First commit shape

```text
feat(ui): inkcell skeleton + --tui flag + agent bridge stub
```

Not a protocol redesign. Not a manifest rewrite. Just the spine.

---

## References

- MK3 TUI: `src/tui/*`, REPL glue `main.cpp` (~2120+)
- Protocol: `docs/protocol/CANON.md`
- inkcell: `../inkcell/DESIGN.md`, `LLM_DOCS.md`
- Manifest manager prototype: `src/tui/manifest_manager.hpp`
- Artifact: `art-mrflxfd4-gupoyt`
