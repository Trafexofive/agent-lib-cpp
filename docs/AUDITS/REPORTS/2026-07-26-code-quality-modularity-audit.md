# Code Quality, Modularity & Reusability Audit — Cortex MK3

**Date:** 2026-07-26  
**Branch:** `feat/inkcell-agentshell` @ `7dcc4ed`  
**Repo:** `/home/mlamkadm/repos/active/agent-lib-cpp`  
**Auditor:** authoritative (code re-read; free discovery artifacts used as index only)  
**Mandate:** quality / readability / scalability / modularity / **REUSABILITY**  
**Out of scope:** implementing extractions; rehashing session or perf audits (cross-link only)

**Cross-links (do not rehash):**
- [2026-07-26-session-management-audit.md](./2026-07-26-session-management-audit.md) — dual-id, dual-writer, SessionController need
- [2026-07-26-speed-performance-audit.md](./2026-07-26-speed-performance-audit.md) — god-model rebuilds, blocking persist, long-session jank

**North star:** without pure, headless-callable modules you cannot automate, serve, or ship a second UI without forking the product.

---

## 0. Executive verdict + grades

| Dimension | Grade | One-liner |
|-----------|:-----:|-----------|
| **Code quality** | **C+** | Vet-fix quality is high in hot spots; structural debt is the tax |
| **Readability** | **C** | Local blocks often clear; files exceed human working set (1.5–2.5k LOC) |
| **Modularity** | **D+** | Directory tree exists; responsibility boundaries do not |
| **Reusability** | **D** | Core logic bound to `Agent` concrete type + TUI assembly; automation starves |
| **Scalability (design)** | **C−** | Features add lines to monoliths; second consumer = copy or include-poison |
| **Testability** | **C** | Parser/session/diff tested; Agent/ShellModel/CLI mostly integration-only |

**Verdict:** Cortex MK3 is a **working research/operator binary with partial extraction** (session lifecycle, tool dispatch stubs, protocol_event_diff, transcript_cache). It is **not yet a library product**. The largest files are not “big because domain is big” — they are **responsibility dumps**. Dual TUI stacks (`src/tui` vs `src/ui`), dual dialog models, dual session writers, and UI→Agent hard includes are the reusability kill-shots.

**Bottom line:** stop growing `agent.cpp`, `main.cpp`, `inkcell_app_model.hpp`, and `main_scene.hpp`. Extract pure modules first; feature work that lands inside those four without an extraction plan is regression against automation.

---

## 1. Monolith autopsy

LOC ground truth (measured):

| LOC | File |
|----:|------|
| 2596 | `src/core/agent.cpp` |
| 2425 | `main.cpp` |
| 1782 | `src/ui/scenes/main_scene.hpp` |
| 1473 | `src/ui/model/inkcell_app_model.hpp` |
| 1259 | `src/workflows/workflow_engine.hpp` |
| 1257 | `src/protocol/parser.cpp` |
| 1091 | `src/core/agent_catalog.hpp` |
| 980 | `src/tui/repl_session.hpp` |

Partial agent split (real, incomplete):

| LOC | File | Role |
|----:|------|------|
| 533 | `agent_session.cpp` | load/save/checkpoint/undo |
| 289 | `agent_tool_dispatch.cpp` | `dispatchTool` et al. |
| 145 | `agent_manifests.cpp` | |
| 59 | `agent_tool_mgmt.cpp` | |
| 23 | `agent_subagents.cpp` | |
| 15 | `agent_env.cpp` | |
| **2596** | **`agent.cpp` still** | constructor, prompt, **entire runLoop**, prompt build, sanitize, helpers |

### 1.1 `Agent` (`agent.hpp` + `agent.cpp`) — runtime god object

**Public surface** (`agent.hpp:56–244`): execution, modes, output accessors, session, tools/feeds/relics, context pin/peek, sub-agents, sandbox, env, workflow human/checkpoint handlers, `sessionMgr()`.

**Still co-located types that are not “Agent”:**
- `ProtocolAction` / `ProtocolResult` / `ProtocolEventKind` / `ProtocolEvent` — `agent.hpp:30–54`
- `extern std::atomic<bool> g_running` — `agent.hpp:28`, defined `agent.cpp:31`

**`agent.cpp` section ownership (headers verified):**
| Region | Lines (approx) | Responsibility |
|--------|----------------|----------------|
| Helpers | 33–221 | xmlAttr, harness path, path split, json path lookup |
| Constructor | 223–310 | provider config, harness load, persona, registerDefaults |
| `prompt()` | 316–366 | session load policy + `runLoop` + dump |
| Core loop | 424–697 region + `runLoop` from 704 | full iteration machine |
| “Tool Dispatch” banner | 699 | mostly still loop/action body; real dispatch in `agent_tool_dispatch.cpp` |
| Prompt building | 2152–2540 | system/user/dynamic context |
| Sanitize | 2542–2592 | XML strip |

**Mixed responsibilities:**
1. Process lifecycle cancel (`g_running`) owned by Agent TU but used by provider + parser + TUI.
2. Protocol domain types live in agent header → every UI include of `agent.hpp` pulls the runtime.
3. Session persistence policy inside Agent + UI + atexit (see session audit).
4. Prompt assembly + loop + tool orchestration still one TU of ~2.6k.

**Why it blocks reuse:** a headless automation client that only needs “build messages → call LLM → parse → dispatch” must link/compile against session manager, tools map, sandbox, ask handlers, and protocol event vectors — no façade.

### 1.2 `main.cpp` (2425) — CLI + session UX + agent factory + TUI launch

Verified section map:
| Lines | Block |
|------:|-------|
| 66–157 | `CliConfig` |
| 161–191 | `Spinner` |
| 195–201 | signal → `g_running=false` |
| 205–322 | help surfaces |
| 326–394 | config file R/W |
| 399–795 | `parseArgs` |
| 833–1145 | session list/mint/resolve/metadata/persist |
| 1149–1220 | resume banner |
| 1226–1508 | `cmdList` / `cmdSessions` |
| 1512–1565 | `cmdConfig` |
| 1570–1629 | `cmdCompletions` |
| 1634–1893 | interactive picker + manifest manager |
| 1897–2360 | **`cmdRun`** — fork, resume, dry-run, agent build, TUI |
| 2364–2375 | `cmdServe` |
| 2379+ | `main` dispatch |

There is **no `src/cli/`**. Every command shares one TU with session fork logic (`main.cpp:1955–1975` region) and full `cmdRun` pipeline. CLI unit tests require the whole binary surface.

### 1.3 `ShellModel` (`inkcell_app_model.hpp` 1473) — domain model + policy + I/O + bridge

File claims “Drawing stays out” (`inkcell_app_model.hpp:2`) — true for pixels; false for purity.

**Co-located in one header:**
| Lines | Concern |
|------:|---------|
| 29–53 | `InkcellAppConfig` |
| 58–139 | display sanitize / truncate utilities |
| 143–179 | `TimelineRow` + glyphs |
| 181–262 | **timeline serialize/deserialize** (JSON) |
| 264–432 | protocol → row adapters (`rowFromProtocol`, `rowsFromAgent`) |
| 434–1473 | **`ShellModel`** (~1k lines) |

**`ShellModel` owns simultaneously:**
- Timeline store + selection + nested drill-down (`agentPath`, `nestedRows`)
- Row policy: body caps, sanitize, focusable rules (`applyRowBans` ~635–684)
- Eviction: `kRootRowCap = 600` + `enforceRowCap` (`607–634`)
- Event reducer: `apply(UiEvent)` (`1030–1249`) including RETRY epoch logic
- Bridge drain/settle ask (`1254–1281`)
- Session load projection + **disk write** `persistUiTimeline` (`1321–1366`)
- Composer history + **lazy session arm + seed + saveSession** (`1414–1470`)
- Dashboard/workflow pending fields, palette, ask dialog state, notifications
- Direct `Agent*` root (`setRootAgent`, sub-agent drill via `rootAgent->getSubAgent`)

Evidence — persistence inside model:
```text
inkcell_app_model.hpp:1342  persistUiTimeline() → sessionMgr().load/save
inkcell_app_model.hpp:1457  rootAgent->saveSession(activeSessionId) on first submit
```

This is not a pure view-model. It is an **application controller** living in a header that every scene includes via `base_scene.hpp`.

### 1.4 `MainScene` (1782) — hub as kitchen sink

Includes drag workflow engine + catalog-adjacent UI + gfx shaders (`main_scene.hpp:13–33`).

Methods verified:
- `on_key` 71–478 — full hub keyboard FSM
- `draw` + `drawAppBar` / `drawHome` / `drawSessions` / `drawManifests` / `drawSettings` / `drawWorkflowStage`
- Session mutations: `resumeSelectedSession`, `createSession`, `killLiveSession`, `exportSelectedSession`, `deleteSelectedSession` (1610–1748 region)
- Workflow queue/resume (1573+)

**Mixed:** input, layout, catalog browsing, session CRUD, workflow stage, prefs. No controller split beyond `dashboard_controller.hpp` (partial).

### 1.5 `AgentScene` (727) — better bounded, still mixed

Owns: key ladder (Esc/notification/timeline/composer), palette, ask keys, slash commands, draw composition, stop loop. Model mutation for navigation lives on `ShellModel`; scene still embeds slash completion policy + ask card finish + agent stop (`agent_scene.hpp:22–711` map). Acceptable size if model stays pure; currently compensates for god-model.

### 1.6 `mk3_tui_app.hpp` (588) — assembly + dual-id flush + atexit

Responsibilities:
- `initializeChatModel`, `installAppTick`, `makeInkcellApp`
- **`cortex::mk3::flush::State`** singleton + atexit (`122–169`)
- `runAgentTurn` (worker thread + retry → Notification)
- `flushAgentSession` (`356–359`)
- `LiveAgentSlot` + `buildAgentFromManifest` (`363–399`)
- `runInkcellShell` / `runInkcellOneShot` / `runInkcellRepl`

Dual-id flush (smoking gun, also session audit):
```text
mk3_tui_app.hpp:158–159  runOnce: flush(cfgSid) + flush(sid)
mk3_tui_app.hpp:276–277  runInkcellShell exit: cfg.sessionId + model->activeSessionId
mk3_tui_app.hpp:344–345  one-shot/repl teardown: same pattern
```

App assembly is the right place for wiring; **session identity policy is not**.

### 1.7 `repl_session.hpp` (980) — legacy TUI monolith

Comment admits 1:1 port (`repl_session.hpp:1–5`). Owns terminal alt-screen, resize, wake fd, agent thread sync, dialog session, slash commands, rendered history save (`saveRenderedHistory` ~810), full loop. Parallel product surface to inkcell — dual stack cost.

### 1.8 `workflow_engine.hpp` (1259) — triple embed

```text
workflow_engine.hpp:26   class MiniYaml { ... }   // orphan YAML parser
workflow_engine.hpp:139  class SchemaValidator
workflow_engine.hpp:216  class WorkflowEngine
```

Meanwhile `src/core/mini_yaml.hpp` already has `ManifestYaml` (299 LOC) used by manifests. Third YAML-ish dialect = three maintenance surfaces.

### 1.9 `agent_catalog.hpp` (1091)

Discovery + resolution + ASCII tree formatting + nested walk in one header. Formatting for TUI should not live next to FHS path resolution if automation only needs resolve.

---

## 2. Dependency & layering

### 2.1 Intended layers (what the tree pretends)

```text
providers / tools / protocol / session / feeds / relics / workflows
        ↑
      core::Agent
        ↑
   bridge / ui.model / ui.chat
        ↑
   ui.scenes / ui.app / main.cpp
```

### 2.2 Actual violations

| Violation | Evidence | Effect |
|-----------|----------|--------|
| **UI event contract includes full Agent** | `ui_event.hpp:12` `#include "src/core/agent.hpp"` for `ProtocolEvent` | Any consumer of `UiEvent` compiles Agent runtime deps |
| **ShellModel includes Agent** | `inkcell_app_model.hpp:17` | Model not unit-testable without Agent |
| **Adapters include Agent** | `protocol_to_timeline.hpp:9`, `agent_tree.hpp:8` | Timeline adapter not pure |
| **Provider includes Agent for cancel** | `generic_openai.cpp:16` `g_running` | Provider layer depends on core agent TU |
| **Parser includes Agent for cancel** | `parser.cpp:14` | Protocol layer depends on Agent |
| **Scenes include workflow engine** | `main_scene.hpp:33` | Hub compile unit = world |
| **protocol_event_diff includes types + ui_event** | `protocol_event_diff.hpp:12–13` | Good isolation *except* ProtocolEvent still lives in `agent.hpp` — diff purity is partial |

### 2.3 Allowed dependency rules (proposed)

| Module may import | Must not import |
|-------------------|-----------------|
| `protocol/*` | `agent.hpp`, UI |
| `session/*` | UI, Agent |
| `core/protocol_types` (new) | Agent methods |
| `ui/bridge` | only protocol_types + json |
| `ui/model` | bridge events, session DTOs — **not** `Agent*` |
| `ui/scenes` | model + inkcell |
| `cli/*` | session, catalog, providers — not scenes |
| `Agent` | tools, protocol, session, providers |

**Current killer edge:** `UiEvent` → `agent.hpp` → session + tools + parser + provider headers. Layering is inverted at the UI boundary.

### 2.4 Lib boundary (Makefile)

```text
Makefile:20  SRCS := find src/**/*.cpp  (excludes testing, call_tool)
Makefile:27  libagent-mk3.so / .a from OBJS
Makefile:40–44  main.cpp → cortex-mk3 only
```

**Good:** main is not in the shared lib.  
**Bad:** lib still ships UI headers’ object code if UI has `.cpp` (mostly header-only UI → compile poison for any TU that includes ShellModel). Consumers of `libagent-mk3` who include `inkcell_app_model.hpp` pull inkcell widgets + Agent + dashboard.

---

## 3. Reusability blockers

North-star consumers that **cannot cleanly share logic today:**

| Consumer | Blocker |
|----------|---------|
| **Headless automation / CI agent** | `prompt()` coupled to session load policy, `g_running`, ask handlers, dump artifacts; no `AgentEngine` façade |
| **HTTP server tooling** | Server reuses Agent but session identity + UI timeline are TUI concepts; no shared SessionController |
| **Second UI** (web, Android, pure REPL) | Timeline serialize, row policy, dialog model, event reduce live in inkcell-shaped ShellModel |
| **Scripted batch over sessions** | Session list/fork/metadata logic buried in `main.cpp` statics |
| **Library embedder** | ProtocolEvent in `agent.hpp`; cancel flag global; dual dialog namespaces |

### Concrete blockers (file:line)

1. **Protocol domain types in Agent header** — `agent.hpp:30–54`  
2. **Global cancel** — `g_running` agent.cpp:31; provider + parser + TUI  
3. **Three session write paths** — `agent_session.cpp:268`, `inkcell_app_model.hpp:1342–1363`, `mk3_tui_app.hpp:356–358` (+ dual id)  
4. **Dialog type fork** — identical structs `tui::Dialog*` vs `ui::chat::Dialog*` (`dialog.hpp:17–69` ≈ `ask_dialog_model.hpp:16–68`; diff of struct blocks empty)  
5. **YAML triple** — `ManifestYaml` vs workflow `MiniYaml`  
6. **Lazy session arm inside ShellModel** — `submitComposer` 1414–1470 — product policy in UI model  
7. **No interfaces** for ToolRegistry / FeedEngine / Reliquary / LlmProvider beyond `LlmProviderPtr` concrete hierarchy  
8. **Header-only mega scenes** — any second binary including MainScene pays full include graph

### What “reusable” would mean here

A pure module is callable from:
- `cortex-mk3 run`
- `cortex-mk3-server`
- a Python/Go binding later
- a test without inkcell or tty

Today only **parser**, **SessionManager**, **mini_yaml (manifest)**, **protocol_event_diff**, **transcript_cache**, and **parts of tools/providers** approach that bar.

---

## 4. What is already modular (praise with paths)

Do not burn these; extend the pattern.

| Asset | Path | Why it works |
|-------|------|--------------|
| **Protocol event diff** | `src/ui/model/protocol_event_diff.hpp` (66 LOC) + `protocol_event_diff_test.cpp` | Pure functions; RETRY contract documented; no Surface/Agent methods |
| **Transcript wrap cache** | `src/ui/chat/transcript_cache.hpp` (40 LOC) | Incremental wrap state; clear invalidate; no I/O |
| **SessionManager** | `src/session/manager.{hpp,cpp}` (53 + 366) | Complete CRUD API; tmp+rename; list/export/import; tested |
| **Agent session split** | `src/core/agent_session.cpp` (533) | load/save/checkpoint out of agent.cpp |
| **Tool dispatch split** | `src/core/agent_tool_dispatch.cpp` (289) | Real dispatch not only a comment |
| **Parser** | `src/protocol/parser.cpp` (1257) | Large but single-domain; heavy tests |
| **Ask dialog model (inkcell)** | `src/ui/chat/ask_dialog_model.hpp` | Renderer-independent validation (good direction) — needs merge with tui |
| **AgentBridge** | `src/ui/bridge/agent_bridge.hpp` | Thread-safe queue + eventfd + ask CV — solid conduit |
| **ScriptedProvider** | `src/testing/scripted_provider.hpp` | Enables loop tests without network |
| **Types Session DTO** | `src/core/types.hpp` Session/SessionRecord | Shared on-disk shape |
| **Makefile lib split** | `libagent-mk3` vs `main.cpp` | Correct binary/lib cut at link level |
| **UI prefs / dashboard model / workflow_run_model** | `src/ui/model/*` | Partial extraction already started |

**Pattern to copy:** small header, pure or nearly pure functions, dedicated test TU, no `Agent*`, documented contracts (see RETRY comments in protocol_event_diff).

---

## 5. Target module map

Proposed tree (names are API targets, not bikeshed):

```text
src/
  protocol/
    events.hpp          # ProtocolAction, ProtocolResult, ProtocolEvent, Kind
    parser.*            # (existing)
    noise.*             # (existing)
  session/
    manager.*           # (existing — store only)
    controller.hpp      # ONE active id, save/load/flush policy
    identity.hpp        # mint, resolve, fork, metadata merge
  core/
    agent.hpp/cpp       # loop + dispatch only
    prompt_builder.*    # buildSystem/User/Chat
    context_store.*     # pin/peek/unpin
    cancel.hpp          # CancellationToken (replaces g_running)
  cli/
    args.hpp            # CliConfig + parseArgs
    config_file.*
    commands/
      run.cpp           # cmdRun
      sessions.cpp
      list.cpp
      config.cpp
      serve.cpp
      completions.cpp
    pickers.*           # provider/model/manifest interactive
  ui/
    model/
      timeline_types.hpp    # TimelineKind, TimelineRow
      timeline_codec.*      # serialize/deserialize
      row_policy.*          # sanitize, body cap, enforceRowCap
      event_reducer.*       # apply(UiEvent) → store mutations
      shell_state.hpp       # thin state POD + selection
      protocol_event_diff.* # (existing)
    chat/
      dialog_model.hpp      # SINGLE Dialog* (merge tui+ui)
      transcript_cache.*    # (existing)
    bridge/
      ui_event.hpp          # depends on protocol/events only
      agent_bridge.hpp      # (existing shape)
  workflows/
    engine.*                # WorkflowEngine only
  utils/
    yaml.hpp                # one YAML parser
    schema_validator.hpp
    text_sanitize.hpp       # sanitizeForDisplay / safeTruncate
```

### Key type moves

| From | To |
|------|-----|
| `ProtocolEvent*` in `agent.hpp` | `src/protocol/events.hpp` |
| `serializeTimeline` / `deserializeTimeline` | `timeline_codec` |
| `enforceRowCap` / body caps | `RowPolicy` |
| `persistUiTimeline` + dual flush | `session::SessionController` |
| `CliConfig` + cmds | `src/cli/` |
| `MiniYaml` + `SchemaValidator` | `utils/` |
| `Dialog*` dual | one `dialog_model.hpp` under `ui/chat` or `ui/model` |
| `g_running` | `CancelToken` injected into Agent + provider + parser |

---

## 6. Extraction backlog

Cost: **S** ≤0.5d · **M** 1–2d · **L** 3–5d.  
Acceptance tests are gates, not vibes.

### P0 — structural integrity (before feature accretion)

| ID | Extract | From | Cost | Acceptance | Couples to |
|----|---------|------|:----:|------------|------------|
| **P0.1** | `protocol/events.hpp` | `agent.hpp:30–54` | **S** | All TUs compile; `protocol_event_diff_test` green; UI no longer needs agent.hpp for events | Unblocks pure UI bridge |
| **P0.2** | Merge Dialog models | `tui/dialog.hpp` + `ui/chat/ask_dialog_model.hpp` | **S** | Single header; both TUIs include it; ask_tool tests pass | Drift stop |
| **P0.3** | One YAML (`utils/yaml` or promote `mini_yaml`) | workflow MiniYaml + ManifestYaml | **S–M** | workflow_engine_test + yaml_test + manifest loads | Dead code removal |
| **P0.4** | `SessionController` | agent save, ShellModel persist, mk3 dual flush | **M–L** | Single active id API; dual-flush deleted; session audit P0 items; lazy_session + live_resume + roundtrip tests | **Session audit** |
| **P0.5** | CLI → `src/cli/` | `main.cpp` | **L** | `main.cpp` <300 LOC dispatch; cmd* unit-testable without TUI link | Automation entry |

### P1 — reusability + testability

| ID | Extract | From | Cost | Acceptance | Couples to |
|----|---------|------|:----:|------------|------------|
| **P1.1** | `TimelineCodec` + `RowPolicy` | inkcell_app_model 76–634 | **M** | ui_timeline_test + perf drain gate; no disk in codec | **Perf audit** (cap/rebuild) |
| **P1.2** | `EventReducer` | `ShellModel::apply/drain` | **M** | 1000-event drain ≤100ms (existing perf gate); unit test without Surface | **Perf audit** |
| **P1.3** | `PromptBuilder` | agent.cpp 2152–2540 | **M** | `renderSystemPrompt` / golden fixtures; Agent calls builder | Headless prompt inspection |
| **P1.4** | `CancelToken` | `g_running` | **M** | Multi-agent tests; provider/parser take token ref | Server concurrency |
| **P1.5** | SchemaValidator extract | workflow_engine | **S** | Pure unit tests | Workflows |
| **P1.6** | Thin ShellModel | leftover state POD | **M** | ShellModel <400 LOC; scenes still work | Follows P1.1–1.2 |

### P2 — ergonomics / scale-of-team

| ID | Extract | From | Cost | Acceptance |
|----|---------|------|:----:|------------|
| **P2.1** | Split MainScene by section | main_scene draw* | **M** | Home/Sessions/Manifests/Settings/Workflow as components |
| **P2.2** | Catalog format vs resolve | agent_catalog | **S–M** | resolve API used by CLI without tree formatter |
| **P2.3** | Lifecycle helpers | atexit/signal | **S** | One install path; no static Agent* without controller |
| **P2.4** | Deprecate / quarantine `src/tui` | repl_session stack | **L** | Feature flag or build toggle; no new code in legacy |
| **P2.5** | Collaborator interfaces | Agent tools/feeds/relics | **M** | Mock registry in tests |

### Mapping to other audits

| This backlog | Session audit | Perf audit |
|--------------|---------------|------------|
| P0.4 SessionController | **implements** dual-writer / dual-id fix | moves disk off ad-hoc UI paths |
| P1.1 RowPolicy / cap | — | **implements** eviction design; enables non-O(n) erase later |
| P1.2 EventReducer | — | preserves batch rebuild; testable jank budgets |
| P0.5 CLI extract | session resolve lives with identity module | — |
| P2.4 dual TUI | renderedHistory vs ui_timeline | dual perf profiles → one path |

---

## 7. Readability / naming / header-only costs

### 7.1 Readability

- **Local quality:** many vet-fix comments are high-signal (RETRY, dual-id, empty wipe). That is good operator archaeology — and a smell that architecture is comment-driven.
- **File-level readability fails** past ~800–1000 LOC. `agent.cpp`, `main.cpp`, `main_scene.hpp`, `ShellModel` require section maps to navigate.
- **Misleading banners:** `agent.cpp:699` “Tool Dispatch” precedes more loop body; actual dispatch is another file — searchers land wrong.

### 7.2 Naming

| Name | Issue |
|------|--------|
| `ShellModel` | Sounds pure; owns disk + agent + policy |
| `flushAgentSession` | Flushes agent records only — not UI timeline (callers must also `persistUiTimeline`) |
| `noSession` vs `ephemeral` | Orthogonal but overloaded in operator mental model (session audit) |
| `MiniYaml` vs `ManifestYaml` | Same idea, two names |
| `g_running` | Process global, not agent instance |

### 7.3 Header-only costs

| Header | Cost |
|--------|------|
| `main_scene.hpp` 1782 | Every chat_scene_test / app TU recompiles hub world |
| `inkcell_app_model.hpp` 1473 | Any model test pays full Agent + inkcell widgets includes |
| `workflow_engine.hpp` 1259 | MiniYaml+Schema+Engine always together |
| `mk3_tui_app.hpp` 588 | Assembly + atexit + run loops inline — slow iteration, weak linker isolation |
| `repl_session.hpp` 980 | Entire legacy TUI in header |

**Rule of thumb:** if a unit has non-trivial logic and is included by >2 TUs, it wants a `.cpp`. Timeline codec and SessionController especially.

### 7.4 Duplication scorecard

| Dup | Severity |
|-----|----------|
| Dialog structs tui vs ui | **High** — identical layout, dual maintenance |
| YAML parsers | **High** — semantic drift risk |
| ActionType enums (TUI protocol component vs parser) | **Medium** (noted in discovery; verify on touch) |
| Session save call sites | **Critical** (correctness, not just style) |
| Dual TUI product surfaces | **Strategic** — every chat feature ×2 |

---

## 8. Scalability of design (team / features / products)

Not runtime microbench (see perf audit). Design-scale:

| Axis | Today | Failure mode |
|------|-------|--------------|
| **Add CLI command** | +main.cpp | File already 2.4k; merge conflicts; no test harness |
| **Add protocol event kind** | agent.hpp + ShellModel::apply + possibly legacy protocol.hpp | Triple paint paths |
| **Add second UI** | Copy ShellModel or include inkcell | Product fork |
| **Multi-agent server** | `g_running` process-wide; Agent owns SessionManager instance | Cross-talk / cancel kills all |
| **Team parallel work** | Everyone touches agent.cpp / inkcell_app_model / main | Serialization of development |
| **Automation product** | Logic trapped in TUI/model | Reimplement in scripts |
| **Long-term features** (workflows hub, feeds UI) | Land in MainScene | Scene → 2.5k+ |

**Scalability grade C−:** the system scales by **file growth**, not by **module composition**. Partial splits prove the team knows how; the remaining god objects absorb all new behavior.

---

## 9. What NOT to split yet

| Leave alone | Why |
|-------------|-----|
| **Parser internals** | Already single-domain; large but cohesive; tests hold the line |
| **inkcell framework** | External boundary; do not “modularize” Surface/Engine into Cortex |
| **provider HTTP details** (`generic_openai.cpp`) | Split only if adding providers; not modularity-critical |
| **Agent runLoop algorithm** (before PromptBuilder/CancelToken) | Extract collaborators first; surgical loop split mid-refactor risks regressions |
| **chat_view rendering** | Bound to inkcell; keep as view after model purify |
| **Full Agent interface explosion** (IToolRegistry…) | P2; SessionController + protocol types deliver more reuse per day |
| **workflow step runtime** while MiniYaml still embedded | Extract YAML/Schema first, then engine .cpp |
| **Big-bang delete of `src/tui`** | Quarantine; kill only when inkcell parity checklist is green |

---

## 10. Sequencing vs feature work

### Extract-as-we-go (recommended)

```text
Rule: any feature that must edit agent.cpp / main.cpp / inkcell_app_model.hpp /
      main_scene.hpp for >~50 LOC of non-feature glue MUST land with a
      P0/P1 extraction slice in the same change set or immediately prior.
```

### Recommended sequence (2–3 weeks of focused modularity, parallelizable)

```text
Week 1:
  P0.1 protocol/events.hpp          (unblocks everything UI)
  P0.2 dialog merge
  P0.3 YAML unify (or delete workflow MiniYaml → ManifestYaml adapter)
  P0.4 SessionController skeleton   (with session audit fixes)

Week 2:
  P1.1 TimelineCodec + RowPolicy
  P1.2 EventReducer
  P1.6 ShellModel thin
  Start P0.5 CLI peel (args + sessions cmd first)

Week 3:
  Finish P0.5 cmdRun move
  P1.3 PromptBuilder
  P1.4 CancelToken
  P2.4 freeze legacy tui (no new features)
```

### Feature work that is safe without extraction

- Parser behavior / protocol noise (tested domain)
- Provider retries (keep CancelToken in mind)
- inkcell-only draw polish inside AgentScene draw helpers
- SessionManager on-disk format extensions **if** written only through manager API

### Feature work that is unsafe without extraction

- New session identity rules (fork/timeline/hub) → **SessionController first**
- New timeline row kinds / eviction → **RowPolicy + codec first**
- New CLI subcommands of non-trivial size → **src/cli first or simultaneous**
- Server multi-tenant cancel → **CancelToken first**

### Definition of done for “modular enough to automate”

1. Headless binary or test can: load session → build prompt → run loop → save session **without** including any `src/ui/**` header.  
2. UI can: reduce protocol events → timeline rows → serialize **without** linking Agent methods (Agent only via bridge worker).  
3. One session write API. One dialog model. One YAML parser. One cancel token type.

---

## References

### Primary evidence (this audit)

| Path | What was verified |
|------|-------------------|
| `src/core/agent.hpp` | Public API; ProtocolEvent; g_running |
| `src/core/agent.cpp` | Section headers; prompt; runLoop ownership; LOC 2596 |
| `src/core/agent_session.cpp` | save/load policy; wipe guards |
| `src/core/agent_tool_dispatch.cpp` | Partial split |
| `main.cpp` | Full section map; cmdRun; no src/cli |
| `src/ui/model/inkcell_app_model.hpp` | Timeline codec; cap; apply; persist; lazy arm |
| `src/ui/scenes/main_scene.hpp` | Include nexus; method list |
| `src/ui/scenes/agent_scene.hpp` | Key/draw responsibilities |
| `src/ui/app/mk3_tui_app.hpp` | Dual-id flush; atexit; LiveAgentSlot |
| `src/session/manager.{hpp,cpp}` | Store API completeness |
| `src/ui/model/protocol_event_diff.hpp` | Exemplar pure module |
| `src/ui/chat/transcript_cache.hpp` | Exemplar pure module |
| `src/ui/chat/ask_dialog_model.hpp` vs `src/tui/dialog.hpp` | Struct-identical dup |
| `src/ui/bridge/ui_event.hpp` | agent.hpp dependency |
| `src/ui/bridge/agent_bridge.hpp` | Conduit design |
| `src/workflows/workflow_engine.hpp` | MiniYaml + Schema + Engine |
| `src/core/mini_yaml.hpp` | ManifestYaml preexisting |
| `Makefile` | SRCS / lib / main split |

### Sibling audits

- `AUDITS/REPORTS/2026-07-26-session-management-audit.md`
- `AUDITS/REPORTS/2026-07-26-speed-performance-audit.md`

### Discovery index only (not trusted as conclusions)

- `.artifacts/modularity-audit-evidence-pack`
- `.artifacts/modularity-audit-agent-lib-cpp`

---

*End of modularity audit. Reusability is the north star — pure modules or perpetual dual maintenance. GODSPEED.*
