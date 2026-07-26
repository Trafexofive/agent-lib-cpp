# Inkcell Overhaul Priority Plan

**Date:** 2026-07-11  
**Status:** Execution order for visual + QoL overhaul  
**Invariant:** `--tui legacy` and `--tui inkcell` stay on ReplSession until the default-flip gate passes.

---

## P0 — Guard Rails (always on)

**Purpose:** prevent another scuffed replacement of the working TUI.

- Keep `tests/tui/repl_parity_smoke.sh ./cortex-mk3` green.
- Do not route `--tui inkcell` to experimental UI.
- Do not render from agent thread.
- Do not mix visual scene state into core agent runtime.
- Every implementation slice must have a narrow verification command.

**Done when:** every later commit reports parity smoke status.

---

## P1 — Model Foundation (first implementation slice)

**Purpose:** build structure without touching rendering/default routing.

Deliverables:

- `TimelineBlock` model
- `AgentRunView`/path model
- protocol event → timeline block adapter
- recursive sub-agent path helpers
- command/context skeleton for future footer/help/palette
- unit tests using synthetic protocol events

Why first: sub-agent drill, block focus, detail panels, palette actions, and state rendering all depend on this. Views without this become string hacks.

**Gate:** model tests + build + ReplSession parity smoke.

---

## P2 — Command Inventory + Context Status

**Purpose:** eliminate hidden commands and duplicated footer/help/palette state.

Deliverables:

- command registry model: id/label/group/scope/safety/enabled reason
- context model: provider/model/session/manifest/run lifecycle
- tests for command availability by focus/view/state

**Gate:** footer/help/palette can derive from same inventory later.

---

## P3 — Fixture-Only Visual Components

**Purpose:** draw serious sbtui blocks without live-agent risk.

Deliverables:

- topbar/footer/timeline/detail/composer view contracts
- snapshot harness for 80/100/120/160 cols
- fixtures for empty/loading/populated/error/partial/nested

**Gate:** DESIGN.md invariants checked in snapshots.

---

## P4 — Experimental AgentHistory Controller

**Purpose:** interactive navigation over fixture/model data.

Deliverables:

- focus modes: composer/history/detail/palette/modal
- navigation stack
- block selection
- recursive drill-down over static agent tree
- contextual help overlay

**Gate:** no live agent yet; snapshot + keyflow tests.

---

## P5 — Live Bridge Integration

**Purpose:** make experimental app run real turns.

Deliverables:

- ReplSession-style worker/snapshot bridge into `AppState`
- prompt send/stream/final
- cancel
- ask_tool modal
- session resume replay

**Gate:** live smoke + no agent-thread rendering.

---

## P6 — Session / Manifest / Provider UX

**Purpose:** make the app a full daily driver.

Deliverables:

- SessionBrowser
- ManifestSurface
- ProviderPicker
- startup Welcome decision tree
- non-secret recents/state persistence

**Gate:** common daily flows no longer require hidden slash commands.

---

## P7 — Default Flip

**Purpose:** replace ReplSession only when the new app is better.

Required:

- all state matrices covered;
- sub-agent drill recursive;
- ask_tool works;
- cancel works;
- session resume works;
- command palette covers actions;
- operator approves comparison against ReplSession.

Only then: `--tui inkcell` flips to new app. `--tui legacy` remains ReplSession.

---

## Immediate Work

Start with **P1 Model Foundation**. It is low risk, testable, and unlocks every later phase.
