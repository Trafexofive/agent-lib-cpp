# Architecture Foundation — Cortex MK3 × inkcell

**Status:** living contract for foundation-first work.  
**Mandate:** modular, reusable, scalable, decoupled, readable, complexity-aware.  
**Rule:** feature work that grows god files without extraction is regression.

Cross-links: `INKCELL_INTEGRATION.md`, modularity audit `2026-07-26`, dual-repo ledger artifact.

---

## 1. Problem (why foundations now)

| Monolith | LOC then → now | Notes |
|----------|-------------:|-------|
| `inkcell_app_model.hpp` (ShellModel) | ~1700 → **~361** | F0–F7 peels; thin composition root |
| `mk3_tui_app.hpp` | ~620 → **~448** | F5 `inkcell_runtime.hpp` owns tick/flush |
| `main_scene.hpp` | ~1900 | hub still large — next peel target |
| `agent.cpp` / `agent.hpp` | ~2600+ | F1 events peeled; loop still fat |
| `main.cpp` | ~2400 | CLI — after UI pure stack |

**Module map (landed):** `timeline_codec`, `timeline_store`, `event_reducer`, `timeline_projection`, `shell_nav_session`, `pending_route`, `inkcell_runtime`, `sanitize`. ShellModel inherits TimelineStore; apply → reduceUiEvent.

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
| **`ui/model/timeline_projection.hpp`** ✅ | rows → transcript lines, dirty/full rebuild | rebuildViews* |
| **`ui/model/shell_nav_session.hpp`** ✅ | nested drill + session load/persist | enterSub/goBack/load* |
| **`ui/model/pending_route.hpp`** ✅ | typed Agent/Main/Quit | string `pendingRoute` |
| **`ui/app/inkcell_runtime.hpp`** ✅ | coalesced tick + SessionFlushGuard | 3× runInkcell* |
| **`ui/model/chat_vm.hpp`** | POD for drawHeader/status/transcript | ad-hoc vm fill in scene |
| **FocusManager dogfood** ✅ | composer/timeline + modal layers | bool soup (timelineFocus remains for projection) |
| **ShellModel** | composition root **~361 LOC** ✅ | was ~1700 god object |

Hub later: `DashboardController` already partial; peel keys from MainScene only after chat stack is clean.

---

## 4. inkcell foundation (paired)

Product must not invent forever. Library track:

| inkcell | Product uses for |
|---------|------------------|
| COOKBOOK: wake_fd + tick coalesce ✅ | Runtime recipe |
| CommandRegistry + KeyHints | help + palette (partial) |
| StatusBar segments + fill_background ✅ | footer metrics |
| Focus stack / modal + FocusScope ✅ | ask + palette |
| Theme roles | graphite/neon packs |
| Typed Action helpers (`action::join` etc.) ✅ | routes |

**Dogfood or upstream** — every chrome PR answers which.

---

## 5. Foundation cut order (iterative, not big-bang rewrite)

Each step: extract → rewire includes → tests green → optional commit.

| Phase | Deliverable | Status |
|------:|-------------|--------|
| **F0** | Save dirty S-pack+sanitize+docs | ✅ |
| **F1** | `protocol/events.hpp` | ✅ |
| **F2** | `sanitize` + timeline codec pure headers | ✅ |
| **F3** | `TimelineStore` + `EventReducer` wired | ✅ |
| **F4** | Projection peel | ✅ |
| **F4b** | Nested drill + session load/persist peel | ✅ |
| **F5** | `InkcellRuntime` RAII | ✅ |
| **F6** | `PendingRoute` + FocusManager dogfood + StatusBar footer | ✅ |
| **F6b** | Modal focus layers palette/help/ask | ✅ |
| **F7** | Thin ShellModel façade &lt;400 LOC | ✅ ~361 — events/composer peeled |
| **F8** | inkcell COOKBOOK + StatusBar/Focus dogfood | ✅ (partial; CommandRegistry help next) |

QoL (Feel/Composer/Nested) only on seams. **markdown-buddy** is inkcell ship proof (parked passable).

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
