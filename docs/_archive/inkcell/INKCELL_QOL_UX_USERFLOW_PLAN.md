# Cortex MK3 QoL / UX / Userflow Overhaul Plan

**Date:** 2026-07-11  
**Status:** Companion plan to `docs/INKCELL_UI_APP_OVERHAUL_PLAN.md`  
**Scope:** Operator experience, workflow friction, discoverability, defaults, recovery, and day-to-day speed.  
**Non-goal:** Visual redesign details; those live in `INKCELL_UI_APP_OVERHAUL_PLAN.md`.

---

## 0. Thesis

Cortex MK3 should feel like a sharp local operator tool:

```text
open → know where you are → choose/run agent → inspect/steer → recover → continue
```

The UX overhaul is not about “more features.” It is about reducing decision friction and making every state obvious:

- What agent/provider/session am I using?
- What can I do now?
- What is running?
- What changed?
- Where did the output go?
- How do I get back?
- How do I recover from failure?

If the operator needs to remember hidden command syntax for common flows, the UX failed.

---

## 1. Core Userflows

### Flow A — First launch / no context

Goal: start a useful session without reading docs.

```text
launch
  → detect provider/auth/session/manifest state
  → show actionable start surface
  → operator picks recent session OR agent/manifest OR provider/model
  → lands in AgentHistory ready to type
```

Required affordances:

- recent sessions list;
- current provider/model chip;
- current agent/manifest chip;
- “Start blank session”;
- “Pick provider/model”;
- “Pick manifest/agent”;
- visible no-auth/missing-config cause.

### Flow B — Daily run

```text
launch → last-used context visible → type prompt → stream → inspect actions/results → continue
```

QoL requirements:

- last provider/model/agent/session remembered;
- prompt field focused by default if context is valid;
- run status visible immediately after Enter;
- user prompt echoed before provider response;
- final response and actions remain navigable.

### Flow C — Resume work

```text
launch → recent sessions → select → rendered transcript restores → continue prompt
```

QoL requirements:

- sessions sorted by updated time;
- show session name, agent, provider/model, turn count, updated time;
- resume should restore scrollback + selected block when possible;
- stale/local replay clearly marked until a new run starts.

### Flow D — Inspect tool/sub-agent behavior

```text
select action/result → detail → open output/copy/jump to sub-agent → back
```

QoL requirements:

- Enter opens the most useful target:
  - agent action/result → drill sub-agent;
  - tool result → detail/output;
  - response/final → detail/copy;
- secondary actions in command palette;
- breadcrumbs never lose path.

### Flow E — Recover from failure

```text
error block → exact cause → retry/copy/debug/open raw → continue
```

QoL requirements:

- error blocks are selectable;
- show provider/tool/auth/session cause separately;
- retry is visible when safe;
- copy raw/error command always available;
- no generic “something went wrong.”

### Flow F — Ask cards / human-in-loop

```text
agent requests input → modal owns focus → answer/skip/cancel → returns exact focus
```

QoL requirements:

- modal progress visible;
- card title/body not cramped;
- choices keyboard navigable;
- cancel behavior explicit;
- result recorded in timeline.

### Flow G — Switch context mid-work

```text
command palette → switch provider/model/manifest/session → preview impact → apply
```

QoL requirements:

- safe context switches preview what changes;
- unsent composer text preserved;
- running operations block destructive context changes;
- recent choices remembered.

---

## 2. Friction Audit

### Current friction points

| Pain | Fix |
|---|---|
| `--tui inkcell` previously meant worse experimental UI | keep ReplSession default until Gate 4 |
| hidden slash commands | command palette + help overlay |
| provider/model context implicit | persistent chips/topbar/footer status |
| action/result details require scroll reading | selectable blocks + detail panel |
| sub-agent traces not navigable | recursive agent path stack |
| session resume opaque | SessionBrowser with rendered history + metadata |
| cancel semantics unclear | explicit running status + cancel key label |
| ask_tool interrupts are modal but mentally separate | same UI state machine and timeline record |
| `/manifests` output is text dump | ManifestSurface with validation/status |
| copy/export flows are ad hoc | command palette actions + toasts |
| raw/debug outputs discoverability low | detail actions and debug palette group |

---

## 3. UX Principles

1. **Default to the next obvious action.** If the operator launched with a valid context, focus composer.
2. **Never strand the operator.** Every modal/overlay has visible back/cancel.
3. **Keep state local and visible.** Provider/model/session/agent are always visible.
4. **Progress must be truthful.** Show waiting provider vs parsing protocol vs running tools vs streaming response.
5. **Errors are objects.** They can be selected, copied, retried, inspected.
6. **Everything discoverable through `?` and `:`.** Slash commands become shortcuts, not the primary UX.
7. **Preserve work-in-progress.** Unsent composer text survives navigation, provider picker, session browser, and palette.
8. **No confirmation theater.** Safe actions execute; destructive actions get distinct confirmation.

---

## 4. Startup / Welcome UX

### Startup decision tree

```text
if terminal width < 80:
  ResizeNotice
else if no provider auth/config:
  Welcome(state=needs_provider)
else if no sessions and no manifest:
  Welcome(state=first_run)
else:
  Welcome(state=ready)
```

### Welcome layout content

Primary actions:

1. Continue last session
2. Start new session
3. Pick agent/manifest
4. Pick provider/model
5. Browse sessions

Secondary:

- open help;
- open settings/config paths;
- quit.

### Remembered context

Persist locally:

```text
last_provider
last_model
last_agent_manifest
last_session_id
last_workspace
last_tui_mode
```

Do not persist secrets here. Auth remains in existing auth stores.

---

## 5. Command Palette UX

### Palette groups

```text
Run
  Send prompt
  Cancel run
  Retry failed action

Navigate
  Agent history
  Open detail
  Back
  Jump sub-agent
  Sessions
  Manifests
  Providers

Copy / Export
  Copy selected block
  Copy raw output
  Copy final response
  Export transcript
  Dump render/debug

View
  Toggle thoughts
  Toggle raw
  Toggle prompts
  Clear view

Context
  Switch provider/model
  Switch manifest/agent
  Rename session
  Fork session

Danger
  Delete session
```

### Palette rules

- Palette only shows actions valid for current context unless “show all” toggled.
- Each action shows safe/destructive class.
- Mutating actions route to preview/confirm.
- Fuzzy filter always visible.
- `Esc` restores prior focus exactly.

---

## 6. Help Overlay UX

Help is contextual, not a static wall.

Sections:

1. Current mode keys
2. Global keys
3. Available block actions
4. Slash command aliases
5. Navigation explanation

Example footer trigger:

```text
? help · : actions · q quit · Ctrl-C cancel
```

Help overlay must include every implemented keybinding. No hidden keys.

---

## 7. Composer UX

### Initial mode

Single-line composer, Enter sends.

### Later multiline mode

Explicit mode toggle, not accidental:

| Key | Action |
|---|---|
| `Alt-Enter` or `Ctrl-J` | insert newline |
| `Enter` | send in single-line mode |
| `Ctrl-S` | send in multiline mode |
| `Esc` | history focus |

Footer must show current composer mode.

### Composer preservation

Unsent text survives:

- opening palette;
- opening help;
- drilling history;
- switching provider picker;
- resizing;
- transient errors.

### Prompt templates

Later QoL:

- command palette: “Insert prompt template”;
- templates stored local file;
- no template engine until real use demands it.

---

## 8. Timeline / Block UX

### Selection behavior

- After sending prompt, selection follows the active turn unless operator manually moves.
- If operator scrolls/selects older block, auto-follow pauses and footer says `follow paused · End resume`.
- `End` jumps to live bottom.
- New events do not yank selection unless follow is active.

### Block actions

Common actions:

| Block | Enter | Other actions |
|---|---|---|
| user prompt | detail/edit-copy | copy, fork from here |
| thought | detail | toggle visibility, copy |
| tool action | detail | copy params, retry if safe |
| agent action | drill child | detail, copy instruction |
| result ok | detail/output | copy output, open artifact |
| result error | detail/error | retry, copy error, open raw |
| response/final | detail | copy markdown/plain |

### Visual feedback

- Pending action block uses `◐` and pending tag.
- Result replaces/links to action via ID.
- Long output is summarized inline, full output in detail.
- Drillable blocks show `↳` and footer hint.

---

## 9. Sub-Agent Userflow

### Navigation model

Sub-agents are full agents. Treat them as recursive AgentHistory scopes.

```text
root
  ↳ reader
      ↳ grep-specialist
  ↳ tester
  ↳ reviewer
```

### Rules

- Breadcrumb always visible: `root / reader / grep-specialist`.
- Nested AgentHistory uses same block/detail controls.
- Composer hidden in nested scope initially.
- “Continue in this sub-agent” is a future command palette action with preview.
- Back restores exact selection in parent.

### Missing/stale children

If an action references a child not loaded:

```text
! sub-agent history unavailable
reason: child agent not persisted in this session
actions: copy instruction · return
```

No crash, no blank page.

---

## 10. Session UX

### SessionBrowser fields

```text
name / id suffix
updated time
turn count
agent/manifest
provider/model
status: local/stale/running/error
```

### Session actions

- Continue
- Rename
- Fork
- Export transcript
- Delete (destructive confirm)

### Session naming QoL

Default names:

```text
<agent> · <first prompt summary> · <date>
```

Allow rename from palette.

### Resume fidelity

On resume:

- render previous history immediately;
- show `stale local replay` until new run begins;
- preserve last selected block if stored;
- otherwise select last final/response block.

---

## 11. Provider / Model UX

### Provider status

Show for each provider/model:

- configured auth yes/no;
- default model;
- last used;
- known context window if available;
- cost/risk tag if known locally.

### Switching provider/model

Preview:

```text
current: openai-codex/gpt-5.5
new:     opencode/deepseek-v4-flash-free
impact: future turns only; current session preserved
```

No live quota probing without explicit operator approval.

### Auth missing

Show exact source expected:

```text
Missing auth for openai-codex
expected: ~/.pi/agent/auth.json openai-codex.access
```

---

## 12. Manifest / Agent UX

### ManifestSurface fields

- manifest name;
- path;
- summary;
- tools count;
- feeds count;
- relics count;
- sub-agents count;
- validation state.

### Actions

- Activate manifest
- Inspect imported surface
- Open sub-agent list
- Validate
- Copy path

### Validation UX

Partial validation must show good + bad manifests together:

```text
✓ coder              3 tools 3 sub-agents
✗ broken-agent       missing context.system path
```

---

## 13. Error / Recovery UX

### Error classification

Classify every error block into:

```text
provider_auth
provider_rate_limit
provider_network
provider_protocol
parser_error
tool_failed
tool_timeout
tool_permission
session_io
manifest_validation
unknown
```

### Error block minimum

```text
✗ provider_auth openai-codex
reason: missing openai-codex.access
next: open provider picker · copy auth path · retry
```

### Retry policy

- Safe retries visible immediately.
- Destructive action retries require preview again.
- Rate-limit retries show countdown if available.
- Do not pretend retry is available when impossible.

---

## 14. Toasts / Feedback UX

Use toasts only for missable success/undo info:

- copied block;
- exported transcript;
- renamed session;
- switched provider;
- undoable safe mutation.

Errors that affect current work are inline blocks or banners, not toast-only.

Toast stack max 3. Collapse extra into `+N more`.

---

## 15. Search / Filter UX

### History search

- `/` enters search mode in history focus.
- Search term visible as chip.
- `n/N` next/previous.
- `Esc` exits search, selection preserved.
- Empty filtered state says what filter matched nothing.

### Palette search

- always fuzzy;
- exact mode later if needed;
- no-match state explicit.

### Session/Manifest search

- fuzzy local filter;
- clear key visible when active.

---

## 16. Copy / Export UX

### Copy variants

For selected block/detail:

- copy summary;
- copy raw payload;
- copy output;
- copy markdown/plain final response;
- copy action XML/protocol.

### Export variants

- transcript markdown;
- transcript raw protocol;
- selected output to file;
- debug dump.

Every export previews target path before writing.

Clipboard fallback:

1. `wl-copy`
2. `xclip`
3. write `/tmp/mk3-copy-*.txt`
4. toast/banner tells exact fallback path

---

## 17. Cancellation UX

### States

```text
idle
waiting provider
streaming response
running tools
waiting human input
cancelling
cancelled
```

### Rules

- Footer always shows cancel key while cancelable.
- First Ctrl-C cancels current operation.
- Second Ctrl-C during cancelling asks/forces quit only if necessary.
- Cancelled turn remains as a selectable block.
- If a tool cannot be cancelled, UI says `cancelling after current tool returns`.

---

## 18. Persistence / Settings UX

### Local state file

Add non-secret UI state:

```json
{
  "lastProvider": "openai-codex",
  "lastModel": "gpt-5.5",
  "lastManifest": "manifests/agents/coder/agent.yml",
  "lastSessionId": "...",
  "preferredTui": "legacy|inkcell|experimental",
  "recentManifests": [],
  "recentProviders": []
}
```

Do not persist volatile orchestrator state. Do not persist secrets.

### Settings commands

- show config paths;
- open/copy auth path;
- reset UI recents;
- export settings.

---

## 19. Performance UX

### Large history

- virtualize timeline list;
- keep scroll/selection stable;
- lazy-load huge outputs in detail;
- show output byte sizes;
- never block typing on markdown render of huge previous output.

### Rendering budget

- typing latency target: <16ms perceived;
- streaming update target: 20–80ms;
- snapshot deterministic;
- no full transcript recompute on every cursor blink.

### Backpressure

If protocol events stream too fast:

- coalesce render updates;
- never drop model events;
- footer may show `render coalescing` only if it affects perceived freshness.

---

## 20. Accessibility / No-Color UX

- selection must have marker + background + color;
- status has glyph + text;
- errors include `error:` label, not red only;
- success includes `✓` plus text;
- all actions keyboard reachable;
- no fast double-tap required.

---

## 21. Implementation Phases

### Phase UX-A — Command inventory

Create a single source of truth for actions:

```text
src/ui/model/command_model.hpp
```

Every action has:

```cpp
id, label, group, scope, safe/destructive, enabled reason, preview builder
```

Exit: help/footer/palette can derive from same action list.

### Phase UX-B — Context/status model

Create:

```text
ProviderContext
SessionContext
ManifestContext
RunLifecycle
```

Exit: topbar/footer can always answer “where am I / what is running?”

### Phase UX-C — Welcome and startup flow

Implement startup decision tree behind experimental app only.

Exit: no-context launch is actionable and honest.

### Phase UX-D — Palette/help/footer unification

Footer and help overlay derive from command inventory.

Exit: no hidden keys.

### Phase UX-E — Error object model

Add error classification and render adapters.

Exit: provider/tool/session/manifest failures render differently.

### Phase UX-F — Session/manifest/provider pickers

Implement as real surfaces with search/filter and state coverage.

Exit: common context switching no longer requires slash commands.

### Phase UX-G — Persistence and recents

Non-secret UI state persisted.

Exit: daily run starts in useful context.

### Phase UX-H — Copy/export polish

Unified copy/export actions with fallback feedback.

Exit: no silent clipboard failure.

---

## 22. Tests / Acceptance

### Unit tests

- command inventory by view/entity;
- destructive classification;
- startup decision tree;
- error classification;
- settings persistence excludes secrets;
- clipboard fallback path selection.

### Snapshot tests

- Welcome first-run/provider-missing/ready;
- Palette open/filter/no-match;
- Help overlay per mode;
- Error block classifications;
- SessionBrowser empty/populated/filtered;
- ProviderPicker auth missing/populated.

### Manual live checklist

- launch with no provider auth;
- launch with last session;
- switch provider/model;
- run prompt;
- cancel provider wait;
- ask_tool modal;
- copy selected output;
- export transcript;
- delete session confirmation.

---

## 23. Anti-Patterns To Reject

- Adding another slash command without command palette/help integration.
- Footer hints hand-maintained separately from real keymap.
- Persisting secrets in UI state.
- Silent clipboard/export fallback.
- Generic error messages.
- Context switch that destroys unsent composer text.
- Prompt send while provider/auth invalid without clear error.
- Modal that does not restore prior focus.
- “Are you sure?” confirmation for safe actions.
- No preview for file/network/local DB mutation.

---

## 24. First Builder Prompt

```text
Implement Phase UX-A and UX-B from docs/INKCELL_QOL_UX_USERFLOW_PLAN.md.

Create command inventory and context/status model only. Do not change ReplSession routing. Do not flip --tui inkcell to experimental. The output should let footer/help/palette derive actions from one model later.

Verification:
- make cortex-mk3
- tests/tui/repl_parity_smoke.sh ./cortex-mk3
- new unit tests for command availability and context/status lifecycle if test harness exists
```

---

## 25. Summary

Visual overhaul gives Cortex shape.

QoL/userflow overhaul gives Cortex *flow*.

The app becomes good when the operator stops thinking about flags, hidden slash commands, missing context, and recovery paths — and starts thinking only about the agent work.
