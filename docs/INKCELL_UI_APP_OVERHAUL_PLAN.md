# Cortex MK3 Inkcell Visual UI + App Overhaul Plan

**Date:** 2026-07-11  
**Status:** Authoritative post-parity overhaul plan  
**Precondition:** `--tui inkcell` already routes to extracted legacy `ReplSession` and has byte-parity smoke against `--tui legacy`.  
**Design authority:** `DESIGN.md` sbtui Design Specification v3.  
**Do not start this plan by replacing ReplSession.** ReplSession is the stable product shell and regression oracle.

---

## 0. North Star

Build a serious Cortex MK3 terminal application, not another app-shell demo.

The right order is:

```text
legacy ReplSession parity  ✅
  ↓
model/state extraction from legacy renderer
  ↓
component contracts + visual grammar
  ↓
Agent/History first-class app surface
  ↓
sub-agent drill-down and action/result detail
  ↓
command palette + manifests/session/provider flows
  ↓
replace legacy paint only when screenshots/goldens prove parity-or-better
```

The application must feel like a protocol-native operator cockpit, but the implementation must remain boringly testable:

```text
entrypoint → controller → views → {layout, model, theme}
```

No backward dependencies. No idle motion. No decorative pulse. No box spam. No secret keys.

---

## 1. Product Definition

### App name

`Cortex MK3`

### Class

Hybrid:

- **API-client** for provider/model calls and remote tools.
- **Local/client-side** for sessions, manifests, prompts, local tools, rendered history, artifacts.

### One-line value proposition

```text
Protocol-native agent control plane for live model runs, tool execution, sub-agent trees, and session history.
```

### What must be better than legacy

Legacy wins today because it is truthful and compact. The overhaul wins only when it adds:

1. structured focusable blocks instead of raw scroll only;
2. sub-agent tree drill-down;
3. action/result detail and previews;
4. command palette for all actions;
5. real density tiers;
6. explicit state coverage;
7. stronger test/golden infrastructure.

If an overhaul screen is prettier but less operationally useful than ReplSession, it fails.

---

## 2. Information Architecture

### 2.1 Entities

| Entity | Canonical list/grid | Canonical detail | Key relationships | Notes |
|---|---|---|---|---|
| Session | Session list | Session transcript/history | contains Turns, Agent Runs, rendered history | persisted local state |
| Turn | Timeline block group | Turn detail | belongs to Session; has protocol events | one user prompt → one model run |
| Agent Run | Agent tree/history node | Agent run detail | parent/child sub-agent runs | root and sub-agents are same entity type |
| Protocol Event | Timeline row/block | Event detail | belongs to Agent Run | thought/action/result/response/final |
| Action Call | Action block list | Action detail + preview/result | may target Tool/Agent/Workflow/Feed/Relic | mutation preview gate lives here |
| Result | Result block list | Result detail/output | resolves Action Call | ok/error/partial |
| Sub-Agent | Agent tree node | Same Agent/History screen scoped to child | child of Agent Run | recursive navigation |
| Manifest | Manifest list | Manifest detail/surface | imports tools/feeds/relics/agents/workflows | local manifest catalog |
| Tool | Tool list | Tool schema/detail | imported by Manifest; called by Action | includes builtin/script tools |
| Provider/Model | Provider/model picker | Provider detail | used by Agent Run | auth/rate/health states |
| Ask Dialog | Modal card chain | current card detail | triggered by Action Call | focus trap |
| Artifact | Artifact list | Artifact detail/preview | produced by tools/agent | local persisted output |
| Command | Command palette item | command preview | acts on focused entity | safe/destructive class |

### 2.2 Actions

| Action | Entity | Class | Preview required | Confirmation | Notes |
|---|---|---:|---:|---:|---|
| Send prompt | Agent Run | Safe | no | no | creates new Turn; visible immediately as pending |
| Cancel run | Agent Run | Safe | no | no | must always be visible while cancelable |
| Drill into sub-agent | Action/Result/Sub-Agent | Safe | no | no | stack push, breadcrumb updates |
| Pop history stack | Agent Run | Safe | no | no | stack pop |
| Open event detail | Protocol Event | Safe | no | no | side/detail panel |
| Copy block/raw/output | Protocol Event/Result | Safe | no | no | toast confirms copy |
| Retry failed run/action | Result/Turn | Safe unless destructive action | yes if action mutates | maybe | preserve original context |
| Run tool from command palette | Tool | Depends | yes | if destructive | exact command/params preview |
| Switch provider/model | Provider/Model | Safe | yes | no | preview config/session impact |
| Load/resume session | Session | Safe | no | no | renders stale/local state if needed |
| Rename session | Session | Safe | yes | no | local metadata mutation |
| Delete session | Session | Destructive | yes | yes | type-confirm or explicit modal |
| Activate manifest | Manifest | Safe | yes | no | preview imported surface diff |
| Edit manifest | Manifest | Safe/destructive by patch | yes | if destructive | no raw write without preview |
| Clear transcript view | View state | Safe | no | no | does not delete session unless explicit |
| Export transcript/artifacts | Session/Artifact | Safe | yes | no | output path preview |

### 2.3 Views enum

These are the only reachable top-level views for the first real app release:

```cpp
enum class AppView {
    Welcome,
    AgentHistory,
    EventDetail,
    CommandPalette,
    ManifestSurface,
    SessionBrowser,
    ProviderPicker,
    AskModal,
    ConfirmModal,
    HelpOverlay,
    ResizeNotice,
};
```

**Important:** `EventDetail`, `AskModal`, `ConfirmModal`, `HelpOverlay`, and `CommandPalette` are overlays/pushed views, not route tabs.

### 2.4 View transition diagram

```text
Welcome
  Enter / 1 ----------------------> AgentHistory(root)
  p ------------------------------> ProviderPicker
  s ------------------------------> SessionBrowser
  m ------------------------------> ManifestSurface
  ? ------------------------------> HelpOverlay
  q ------------------------------> quit

AgentHistory(path=root or sub-agent path)
  type + Enter -------------------> AgentHistory(root, running turn)
  Esc while composer --------------> history block focus
  i ------------------------------> composer focus (root only)
  j/k, arrows --------------------> move block selection
  Enter on agent action/result ----> AgentHistory(path + child)
  Enter on event ------------------> EventDetail(selected event)
  Backspace/Esc in nested ---------> AgentHistory(parent path)
  : / Ctrl-K ----------------------> CommandPalette(context=selection)
  ? ------------------------------> HelpOverlay
  Ctrl-C / cancel key -------------> cancel running operation

EventDetail
  Esc / Backspace -----------------> AgentHistory(same path/selection)
  c ------------------------------> copy selected field/output
  : / Ctrl-K ----------------------> CommandPalette(context=event)

CommandPalette
  type filter ---------------------> CommandPalette(filtering)
  Enter safe command --------------> execute / toast / return
  Enter mutation command ----------> ConfirmModal or preview detail
  Esc -----------------------------> previous view

ManifestSurface
  Enter manifest ------------------> Manifest detail/preview
  a ------------------------------> activate manifest preview
  Esc -----------------------------> Welcome or AgentHistory

SessionBrowser
  Enter session -------------------> AgentHistory(session root)
  d ------------------------------> ConfirmModal(delete session)
  Esc -----------------------------> Welcome or AgentHistory

ProviderPicker
  Enter model ---------------------> Welcome/AgentHistory with provider set
  Esc -----------------------------> previous view

AskModal / ConfirmModal / HelpOverlay
  Esc -----------------------------> restore exact previous focus/selection
```

---

## 3. Visual Language Contract

### 3.1 Overall grammar

Follow `DESIGN.md`:

```text
page inset(2)
├── topbar: title, session/provider/status chips
├── body: tier-dependent history + detail/action area
└── footer: contextual keys left, global status right
```

### 3.2 Explicit non-negotiables

- No content touches terminal edges.
- No per-row borders.
- No route-sidebar unless the current density tier and IA actually need it.
- No idle animation.
- No app-header decoration replacing useful status.
- No “scene pages” that contain placeholder text.
- Every key that works appears in footer or help.
- Color never carries meaning alone.

### 3.3 Glyph vocabulary

Use exactly this initial MK3-local set:

| Glyph | Meaning |
|---|---|
| `●` | live/running/connected |
| `○` | idle/disconnected |
| `◐` | partial/degraded/in-progress |
| `✓` | success/result ok |
| `✗` | error/result failed |
| `!` | warning/attention |
| `>` | selected block/focus marker |
| `│` | selected body continuation |
| `…` | truncated/more/streaming |
| `↳` | drillable child/sub-agent |

Do not add glyphs ad hoc. Extend this table first.

### 3.4 Theme tokens

Keep local tokens in `src/ui/theme/cortex_theme.hpp` until a second app consumes them.

Minimum required tokens:

```text
base_bg
panel_bg
panel_2
panel_3
text
dim
bright
cyan
green
amber
red
hot_bg
selected_style
```

No shared registry promotion during this project.

---

## 4. Density Tiers

### Wide ≥160 columns

```text
topbar

agent tree / nav     timeline blocks                         detail / action preview
(path + children)    selected turn/event stream               selected event, output, preview

footer
```

- Three panes.
- Left pane shows agent path tree and session stats.
- Middle pane is timeline list.
- Right pane is selected block detail/action preview.

### Standard 100–159 columns

```text
topbar

timeline blocks                                      detail drawer/right panel

footer
```

- Two panes.
- Agent path is breadcrumb in topbar, not separate tree.
- Detail panel collapses to 34–45 cols depending width.

### Narrow 80–99 columns

```text
topbar

single active pane: timeline OR detail OR palette

footer
```

- Stack-based navigation.
- `Enter` pushes detail, `Esc/Backspace` pops.
- No side panes.
- Breadcrumb always visible.

### Below 80 columns

Render only:

```text
CORTEX MK3 requires at least 80 columns.
Current: <w>x<h>
Resize terminal to continue.
```

No broken layout attempt.

---

## 5. State Coverage Matrix

Every reachable view must implement these before it is considered shippable.

### Welcome

| State | Rendering |
|---|---|
| first-run | concise setup options: Provider, Manifest, Session, Start |
| populated | recent session + configured provider/model + primary Start |
| error | config/auth issue with exact cause and action |
| loading | only for config/session scan >300ms; skeleton matching options |

### AgentHistory

| State | Rendering |
|---|---|
| empty genuine | composer + “No turns yet. Type a prompt.” |
| loading cold | prompt accepted, no events yet: user prompt stays visible + status `waiting provider` |
| loading refresh | prior history remains, new pending turn at bottom |
| populated | timeline blocks with counts and selected block |
| partial | successful events render; pending actions show inline `◐`; failed actions show `✗` |
| stale | session replay state marked local/stale until new run starts |
| error | error block in timeline with exact provider/tool cause |
| rate-limited | inline provider block with backoff/cooldown if known |
| offline | footer global status, no generic error |
| permission denied | explicit auth/permission cause in provider/tool block |

### EventDetail

| State | Rendering |
|---|---|
| empty | impossible; if selected event missing, show “event no longer available” |
| loading | large output load skeleton if artifact/output lazy-loaded |
| populated | metadata + body/output + related jump targets |
| error | failed to load output/artifact cause |

### CommandPalette

| State | Rendering |
|---|---|
| closed | absent |
| open | fuzzy list of available actions + route actions |
| filtering | filter chip and live result count |
| no match | explicit no-match with clear key |
| mutation selected | preview required before execution |

### ManifestSurface

| State | Rendering |
|---|---|
| loading cold | scanning manifests skeleton |
| empty genuine | no manifests found, show expected paths |
| populated | manifest list + imported surface summary |
| error | YAML/validation error with path + line if known |
| partial | valid manifests visible; invalid manifests inline marked |

### SessionBrowser

| State | Rendering |
|---|---|
| loading cold | session index skeleton |
| empty genuine | no sessions yet |
| populated | sessions list with updated time/turn count/provider |
| error | exact session store error |
| filtered empty | filter shown + clear key |

### ProviderPicker

| State | Rendering |
|---|---|
| loading | provider/model scan skeleton |
| populated | provider list/model list with auth status |
| auth missing | explicit missing auth source, no generic failure |
| rate-limited | if known from cached status, show cooldown |

---

## 6. Component Contracts

### 6.1 TopBar

Inputs:

```cpp
struct TopBarModel {
    std::string title;
    std::string subtitle;
    std::vector<Chip> chips;
    HealthState providerHealth;
    std::string breadcrumb;
};
```

States: static, degraded, running.

### 6.2 KeyFooter

Inputs:

```cpp
struct KeyFooterModel {
    std::vector<KeyHint> localHints;
    std::vector<KeyHint> globalHints;
    std::string globalStatus;
    int pendingOps;
};
```

Must always show contextual keys and global status.

### 6.3 TimelineBlockList

Inputs:

```cpp
struct TimelineBlockListModel {
    std::vector<TimelineBlock> blocks;
    int selectedIndex;
    bool focused;
    ScrollState scroll;
};
```

Visual states:

- empty;
- loading-with-prior;
- selected row with `panel_3 + green/bright + > marker`;
- unfocused selected row with elevated background only;
- per-block pending/error markers.

No per-row borders.

### 6.4 TimelineBlock

Inputs:

```cpp
struct TimelineBlock {
    std::string stableId;
    BlockKind kind;       // user, thought, action, result, response, error, status
    BlockStatus status;   // idle, pending, ok, error, partial
    std::string title;
    std::string summary;
    std::vector<std::string> tags;
    bool drillable;
    bool hasDetail;
    RelatedTarget related;
};
```

Rendering:

```text
> ✓ result #a1 fs_read                 22ms  1.2KB
│ summary text here...
│ tags: tool fs_read ok
```

### 6.5 DetailPanel

Inputs: selected `TimelineBlock`, full protocol payload, output preview, related targets.

Sections:

- metadata;
- payload/body;
- output/result;
- related jumps;
- command preview if selected action can mutate.

### 6.6 Composer

Inputs:

```cpp
struct ComposerModel {
    std::string value;
    int cursor;
    bool focused;
    bool disabled;
    std::string placeholder;
    std::string modeLabel;
};
```

Rules:

- root AgentHistory only.
- nested sub-agent history is browse-only unless explicitly “continue in child” is implemented later.
- Enter send in single-line mode initially.
- Multiline mode later via explicit key and visible mode indicator.

### 6.7 CommandPalette

Inputs: actions available for current view/entity.

Must include:

- route actions;
- copy actions;
- retry/cancel when applicable;
- jump-to-related;
- manifest/session/provider actions.

### 6.8 Modal / Confirm

Only for destructive or high-cost operations. Must show:

```text
selected: <entity>
operation: <exact action>
class: destructive
preview: <diff/tree/command>
confirm: type <specific word> or explicit distinct confirmation
```

---

## 7. Model Architecture

Create a new app model layer that is independent of rendering and independent of legacy ANSI lines.

```text
src/ui/model/
  app_state.hpp
  session_model.hpp
  agent_run_model.hpp
  timeline_model.hpp
  command_model.hpp
  provider_model.hpp
  manifest_model.hpp
```

### Core model types

```cpp
struct AppState {
    AppView view;
    FocusPath focus;
    NavigationStack nav;
    SessionState session;
    AgentRunState rootRun;
    ProviderState provider;
    ManifestState manifest;
    CommandPaletteState palette;
    ToastState toasts;
    ModalState modal;
};
```

```cpp
struct NavigationStackEntry {
    AppView view;
    std::vector<std::string> agentPath;
    std::string selectedBlockId;
    int scrollOffset;
    FocusPane focusedPane;
};
```

```cpp
struct AgentRunState {
    std::string agentName;
    std::string sessionId;
    std::vector<std::string> path;
    std::vector<TimelineBlock> blocks;
    RunLifecycle lifecycle; // idle, waitingProvider, streaming, runningTools, complete, cancelled, error
    int pendingActions;
    std::map<std::string, AgentRunState> children;
};
```

### Data sources

- `Agent::protocolEvents()` for live root and child runs.
- `Agent::getSubAgent(name)` recursively for sub-agent history.
- `session::SessionManager` for sessions/rendered history.
- `ManifestLoader` for manifest surface.
- provider registry/factory for provider/model availability.

### Derived model adapters

Do not render directly from `Agent`.

Add adapters:

```text
src/ui/model/adapters/
  protocol_to_timeline.hpp
  session_to_view.hpp
  manifest_to_view.hpp
  provider_to_view.hpp
```

This is where protocol events become `TimelineBlock` objects.

---

## 8. Rendering Architecture

```text
src/ui/theme/cortex_theme.hpp     # tokens only
src/ui/layout/sbtui_layout.hpp    # measurement/rect/density only
src/ui/views/
  topbar_view.hpp
  footer_view.hpp
  timeline_view.hpp
  detail_view.hpp
  composer_view.hpp
  palette_view.hpp
  modal_view.hpp
  state_views.hpp
src/ui/scenes/
  app_controller.hpp              # key dispatch + view transitions
```

Views are pure-ish:

```cpp
void drawTimeline(Surface&, Rect, const TimelineBlockListModel&, const Theme&);
```

Views do not mutate app state. Key handling lives in controller/app.

### Snapshot mode

All transitions resolve immediately. No time-based rendering. Snapshot output must be deterministic.

### Live mode

Motion only for:

- view push/pop;
- selection movement;
- list reflow after filter/search;
- new protocol block insertion.

No motion for idle state or static chrome.

---

## 9. Relationship to ReplSession

ReplSession remains the stable fallback and oracle.

### During overhaul

| Command | Meaning |
|---|---|
| `--tui legacy` | ReplSession stable product |
| `--tui inkcell` | ReplSession until new app reaches gate |
| `--tui experimental` or `MK3_TUI_EXPERIMENTAL=1` | new app shell under active development |

Do **not** flip `--tui inkcell` to the new app until the release gate passes.

### Release gate to replace ReplSession as `--tui inkcell`

Required:

- all views in §2.3 have loading/empty/error/populated states;
- AgentHistory root can send prompt and stream live;
- action/result cards are at least as informative as legacy ProtocolView;
- sub-agent drill works recursively;
- ask_tool modal works;
- cancel works;
- session resume works;
- snapshot fixtures exist for every state;
- legacy vs new visual diff reviewed and accepted.

---

## 10. Implementation Phases

### Phase A — Quarantine and naming cleanup

Goal: stop confusing experimental UI with the real path.

Work:

1. Keep `src/ui/**` but mark it experimental in code comments/docs.
2. Add explicit `--tui experimental` only if needed for live testing.
3. Ensure `legacy`/`inkcell` remain ReplSession.
4. Update docs so no one tests old Welcome/History scene as the main path.

Exit:

- `tests/tui/repl_parity_smoke.sh` still passes.

### Phase B — App model extraction

Goal: build the real model without drawing it yet.

Work:

1. Add `TimelineBlock`, `AgentRunState`, `NavigationStack`, `AppState`.
2. Add `protocol_to_timeline` adapter.
3. Add recursive sub-agent traversal adapter.
4. Add unit tests using synthetic protocol events.

Exit tests:

- action events become drillable blocks when `type=agent` and child exists;
- result events link to action IDs;
- nested child paths resolve;
- hidden thoughts toggle is model-level, not view hack.

### Phase C — Timeline renderer fixtures

Goal: draw blocks correctly before live agent wiring.

Work:

1. Implement `timeline_view.hpp` and `detail_view.hpp` against fixed fixtures.
2. Add snapshot render harness for 80, 100, 120, 160 cols.
3. Add state fixtures: empty/loading/populated/error/partial/nested.

Exit:

- no edge-touching;
- no per-row borders;
- selected blocks use three cues;
- footer shows all active keys.

### Phase D — AgentHistory interactive shell

Goal: root AgentHistory is usable with fake/static data.

Work:

1. Controller focus states: composer/history/detail.
2. Keymap: Enter, Esc, i, j/k, arrows, Backspace, ?, :, q.
3. Navigation stack push/pop.
4. Detail open/close.
5. Density tiers.

Exit:

- fixture app can navigate root → child → sub-child → back;
- resize preserves selection/focus;
- help overlay lists current keys.

### Phase E — Live bridge integration

Goal: real agent run on new app path, still not default.

Work:

1. Reuse ReplSession’s proven agent-thread snapshot strategy.
2. Publish snapshots into `AppState` via model adapters.
3. Composer sends prompt.
4. Cancel path uses `g_running=false` or future cancellation primitive.
5. ask_tool modal blocks agent thread via CV and renders as `AskModal`.

Exit:

- prompt → stream → final works;
- tool action/result cards update live;
- ask_tool works;
- cancel works;
- no rendering from agent thread.

### Phase F — Sub-agent history drill

Goal: recursive agent history navigation.

Work:

1. Build child map from `Agent::subAgentNames()` and `getSubAgent()`.
2. Mark agent action/result blocks with `↳` only when child exists.
3. Enter pushes `agentPath + child`.
4. Breadcrumb is full path.
5. Nested views are browse-only first; “continue in child” later via command palette.

Exit:

- root → reader → nested child works;
- missing child gracefully shows stale/missing state;
- selection restored on pop.

### Phase G — Command palette and mutation previews

Goal: all actions discoverable and previewed.

Work:

1. Palette opens with `:`/`Ctrl-K`.
2. Actions generated from current entity/view.
3. Safe actions execute with toast.
4. Mutating actions render preview before execution.
5. Destructive actions require confirm modal.

Exit:

- no hidden command;
- preview gates every mutation.

### Phase H — Session/Manifest/Provider surfaces

Goal: app feels complete without bloating AgentHistory.

Work:

1. SessionBrowser from `SessionManager`.
2. ManifestSurface from catalog/loader.
3. ProviderPicker from provider registry/auth hints.
4. All surfaces pass state coverage matrix.

Exit:

- user can start from Welcome, pick provider/model, resume session, inspect manifest, then run.

### Phase I — Default flip gate

Goal: make new app `--tui inkcell` only when better than ReplSession.

Work:

1. Add comparison report: legacy vs new for core flows.
2. Run live model smoke.
3. Run ask_tool smoke.
4. Run session resume smoke.
5. Run snapshot fixtures.
6. Flip `--tui inkcell` only after review.

Exit:

- ReplSession remains available as `--tui legacy`.

---

## 11. Testing Strategy

### Unit tests

- protocol event → timeline block adapter;
- navigation stack push/pop;
- density tier calculation;
- command availability for view/entity;
- mutation classification.

### Snapshot tests

Fixtures:

```text
agent_empty_80
agent_loading_cold_100
agent_populated_actions_120
agent_partial_error_120
agent_nested_subagent_100
agent_wide_detail_160
palette_filter_nomatch_100
manifest_invalid_partial_120
provider_auth_missing_100
resize_notice_60
```

### Integration smokes

- `tests/tui/repl_parity_smoke.sh` stays as guard for stable path.
- New experimental app smoke is separate and must not replace parity smoke.
- Live provider smoke requires explicit operator approval unless already allowed in session.

### Manual live checklist

- Alt-screen restore.
- Resize live.
- Cancel provider wait.
- Cancel tool run.
- Ask modal.
- Sub-agent drill.
- Session resume.

---

## 12. File/Module Plan

### Keep

```text
src/tui/repl_session.hpp
src/tui/renderer.hpp
src/tui/session_view.hpp
src/tui/status_prompt.hpp
src/tui/dialog.hpp
```

### Add/replace under `src/ui`

```text
src/ui/model/app_state.hpp
src/ui/model/timeline_model.hpp
src/ui/model/agent_run_model.hpp
src/ui/model/command_model.hpp
src/ui/model/adapters/protocol_to_timeline.hpp
src/ui/model/adapters/agent_tree.hpp
src/ui/views/topbar_view.hpp
src/ui/views/footer_view.hpp
src/ui/views/timeline_view.hpp
src/ui/views/detail_view.hpp
src/ui/views/composer_view.hpp
src/ui/views/palette_view.hpp
src/ui/views/modal_view.hpp
src/ui/views/state_views.hpp
src/ui/app/cortex_app.hpp
src/ui/app/cortex_controller.hpp
src/ui/app/cortex_keymap.hpp
```

### Existing experimental files

Either refactor into the names above or leave unused until replaced. Do not build on the current placeholder scene split as-is.

---

## 13. Concrete Keymap v1

Global:

| Key | Action |
|---|---|
| `q` | quit if safe / prompt if running |
| `Ctrl-C` | cancel running; second press quit |
| `?` | help overlay |
| `:` / `Ctrl-K` | command palette |
| `Esc` | back/cancel current mode |

AgentHistory root:

| Key | Focus | Action |
|---|---|---|
| `Enter` | composer | send prompt |
| `Esc` | composer | focus history |
| `i` | history | focus composer |
| `j/k`, arrows | history | move selection |
| `Enter` | history | open detail or drill if primary target is child |
| `Backspace` | nested | pop agent path |
| `r` | history/detail | toggle raw/detail output |
| `t` | history | toggle thoughts |
| `c` | block/detail | copy selected block |

Palette:

| Key | Action |
|---|---|
| text | filter |
| Enter | execute/open preview |
| Esc | close |

---

## 14. Release Gates

### Gate 1 — Model gate

- adapters tested;
- sub-agent recursion tested;
- no rendering dependency in model.

### Gate 2 — Fixture visual gate

- snapshot fixtures across density tiers;
- no DESIGN.md invariant violations.

### Gate 3 — Live experimental gate

- prompt/send/stream/final;
- cancel;
- ask_tool;
- sub-agent drill;
- session resume.

### Gate 4 — Default flip gate

- operator approves visual/interaction comparison against ReplSession;
- `--tui inkcell` flips to new app;
- ReplSession remains `--tui legacy`.

---

## 15. Anti-Patterns To Reject

- Replacing ReplSession before Gate 4.
- Shipping route pages with placeholder text.
- Implementing TextArea composer before timeline/detail/state fixtures.
- Drawing every block in boxes.
- Using color-only selection.
- Adding idle animation.
- Hiding keys in docs only.
- Rendering from the agent thread.
- Making sub-agent drill a string hack instead of model path stack.
- Implementing mutation commands without preview.

---

## 16. First Builder Prompt

```text
Implement Phase B of docs/INKCELL_UI_APP_OVERHAUL_PLAN.md only.

Create the model/adapters for TimelineBlock, AgentRunState, AppState navigation stack, and protocol_to_timeline conversion. Do not touch ReplSession behavior. Do not route --tui inkcell to the experimental app. Add focused unit tests for protocol events -> timeline blocks and recursive sub-agent path resolution.

Verification:
- make cortex-mk3
- tests/tui/repl_parity_smoke.sh ./cortex-mk3
- relevant new model tests

No visual redesign in this phase.
```

---

## 17. Summary

The new app is built behind the stable ReplSession wall.

ReplSession is not the enemy. It is the oracle.

The overhaul succeeds only when the new inkcell app can prove, with fixtures and live checks, that it preserves legacy truth while adding real structure: focusable blocks, detail panes, recursive sub-agent history, command previews, and complete state coverage.
