# Architecture Foundation — Cortex MK3 × inkcell

**Status:** living contract for foundation-first work.  
**Mandate:** modular, reusable, scalable, decoupled, readable, complexity-aware.  
**Rule:** feature work that grows god files without extraction is regression.

Cross-links: `INKCELL_INTEGRATION.md`, modularity audit `2026-07-26`, dual-repo ledger artifact.

---

## 1. Problem (why foundations now)

| Monolith | LOC (approx) | Mixed responsibilities |
|----------|-------------:|------------------------|
| `inkcell_app_model.hpp` (ShellModel) | ~1700 | events, rows, projection, session arm, dashboard glue, agent* |
| `main_scene.hpp` | ~1900 | hub keys, sessions, workflows, draw |
| `mk3_tui_app.hpp` | ~620 | 3 run paths, tick, atexit, worker |
| `agent.cpp` / `agent.hpp` | ~2600+ | loop + protocol types + cancel |
| `main.cpp` | ~2400 | CLI + session + TUI launch |

Partial extractions exist (`SessionController`, `transcript_cache`, `protocol_event_diff`, chat modules) but **new behavior still lands in the four dumps**.

---

## 2. Target layering

```text
┌─────────────────────────────────────────────────────────────┐
│  inkcell (framework)                                        │
│  Engine · Scene · Surface · Theme roles · Commands · Focus  │
│  Widgets (TextArea, ScrollView, StatusBar, …)               │
│  NO cortex domain                                           │
└──────────────────────────▲──────────────────────────────────┘
                           │ dogfood / upstream only
┌──────────────────────────┴──────────────────────────────────┐
│  product UI (agent-lib src/ui)                              │
│  Runtime · Scenes (thin) · ViewModels · pure draw helpers   │
└──────────────────────────▲──────────────────────────────────┘
                           │ UiEvent only
┌──────────────────────────┴──────────────────────────────────┐
│  UI domain (pure, headless-testable)                        │
│  EventReducer · TimelineStore · Projection · RowPolicy      │
│  Dialog model · sanitize · serialize                        │
│  NO inkcell Surface · NO Agent* in pure modules             │
└──────────────────────────▲──────────────────────────────────┘
                           │ bridge / ports
┌──────────────────────────┴──────────────────────────────────┐
│  domain runtime                                             │
│  Agent · SessionController · Protocol · Tools · Workflows   │
│  protocol/events.hpp (not in agent.hpp)                     │
└─────────────────────────────────────────────────────────────┘
```

### Dependency rules

| Module | May depend on | Must NOT depend on |
|--------|---------------|--------------------|
| `protocol/*` | std, json | Agent, UI, inkcell |
| `session/*` | protocol types, fs | inkcell, ShellModel |
| `ui/model/*` pure | protocol events, std | Agent*, Surface, Scene |
| `ui/bridge/*` | protocol, session ids | Scene draw |
| `ui/chat/*` draw | inkcell Surface, pure models | Agent* |
| `ui/scenes/*` | models, bridge, inkcell Scene | agent.cpp internals |
| `ui/app/*` | scenes, session, Agent port | — |
| inkcell | nothing cortex | — |

**Port for Agent:** scenes/runtime hold `Agent*` or a thin `IAgentRuntime` later; pure reducers never include `agent.hpp`.

---

## 3. Target modules (product UI)

| Module | Responsibility | From today |
|--------|----------------|------------|
| **`protocol/events.hpp`** | ProtocolEventKind, ProtocolEvent, Action/Result POD | peel from `agent.hpp` |
| **`ui/text/sanitize.hpp`** | terminal-safe text, caps | ShellModel free functions |
| **`ui/model/timeline_store.hpp`** | rootRows/nestedRows, caps, push, select | ShellModel |
| **`ui/model/event_reducer.hpp`** | UiEvent → store mutations | `ShellModel::apply/drain` |
| **`ui/model/projection.hpp`** | rows → transcript lines, dirty/full rebuild | rebuildViews* |
| **`ui/model/chat_session_policy.hpp`** | lazy arm, persist enqueue (calls SessionController) | submitComposer tail |
| **`ui/model/chat_vm.hpp`** | POD for drawHeader/status/transcript | ad-hoc vm fill in scene |
| **`ui/app/runtime.hpp`** | one RAII: bridge, tick, wake, atexit, flush | 3× runInkcell* |
| **`ui/focus.hpp`** | single enum: Composer / Timeline / Modal / Hub* | bool soup |
| **`ui/routes.hpp`** | enum Route + post_action helpers | `pendingRoute` string |
| **ShellModel** | thin façade / composition root **&lt;400 LOC** | god object |

Hub later: `DashboardController` already partial; peel keys from MainScene only after chat stack is clean.

---

## 4. inkcell foundation (paired)

Product must not invent forever. Library track:

| inkcell | Product uses for |
|---------|------------------|
| COOKBOOK: wake_fd + tick coalesce | Runtime recipe |
| CommandRegistry + KeyHints | help + palette |
| StatusBar slots | footer metrics |
| Focus stack / modal (grow) | ask + palette |
| Theme roles | graphite/neon packs |
| Typed Action helpers (grow) | routes |

**Dogfood or upstream** — every chrome PR answers which.

---

## 5. Foundation cut order (iterative, not big-bang rewrite)

Each step: extract → rewire includes → tests green → optional commit.

| Phase | Deliverable | Exit criteria |
|------:|-------------|----------------|
| **F0** | Save dirty S-pack+sanitize+docs (explicit paths) | clean intentional tree for foundation |
| **F1** | `protocol/events.hpp` | UI can include events without full Agent API; tests compile |
| **F2** | `sanitize` + timeline serialize in pure headers | unit tests; ShellModel includes them |
| **F3** | `TimelineStore` + `EventReducer` | drain/apply tests without Surface; ShellModel delegates |
| **F4** | `Projection` (incremental stays) | rebuildViews tests; perf gates hold |
| **F5** | `InkcellRuntime` RAII | one run path; OneShot/Repl/Shell share |
| **F6** | `Focus` enum + `Route` enum | no new string routes; scenes use helpers |
| **F7** | Thin ShellModel façade | file &lt;400 LOC or split files with clear names |
| **F8** | inkcell COOKBOOK + command registry dogfood | help/status start using lib |

QoL (Feel/Composer/Nested) **after F3** at earliest — on seams, not into the god object.

---

## 6. Non-goals (this foundation track)

- Hard-delete `src/tui/*`  
- Full `main.cpp` CLI peel (P0.5) — after UI pure stack  
- Async LLM / curl_multi  
- Multi-line composer product redesign  
- Rewriting workflow canvas  

---

## 7. Complexity budget

| File / type | Soft cap | Hard stop |
|-------------|----------|-----------|
| Pure model header | 400 LOC | 600 |
| Scene | 500 LOC | 800 |
| Runtime / app assembly | 400 LOC | 600 |
| God façade | 400 LOC | must split |

Over hard stop → extract before merge.

---

## 8. Verify per phase

```bash
make inkcell-lib
make cortex-mk3 test-chat-scene test-ui-model test-session-controller test-perf -j$(nproc)
# add unit targets as modules appear: test-event-reducer, etc.
```

---

## 9. Naming

- `cortex::mk3::protocol` — events  
- `cortex::mk3::session` — SessionController (exists)  
- `cortex::mk3::ui::model` — pure store/reducer/projection  
- `cortex::mk3::ui` — bridge, scenes, runtime  
- inkcell — unchanged namespaces  

Avoid: `utils.hpp`, `helpers.hpp`, `manager2.hpp`.
