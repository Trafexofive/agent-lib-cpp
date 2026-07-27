# Nested Drill Pack

**Goal:** Sub-agent chat feels like a real place, not a swapped panel with root lies.  
**Effort:** ~2–3 hours.  
**Risk:** Medium (metrics wiring; don’t break root performance).  
**Audit refs:** #19–24, #48 + micro-slide.

## Problem

When drilled into a child:

- Header path may be OK, but **identity / provider / metrics often still root**
- Hints still describe root composer shortcuts
- Enter/back is instant — no spatial cue
- Drillable rows are easy to miss

## In scope

| # | Change | Primary files |
|---|--------|----------------|
| N1 | When `!atRoot()`, header/status use **current** agent name; show `← Esc/Back` affordance | `agent_scene.hpp`, `chat_view.hpp` |
| N2 | Nested metrics: pending/actions/results from child protocol stream **or** explicit `n/a · nested` | `inkcell_app_model.hpp`, `agent_scene.hpp` vm fill |
| N3 | Nested hint string: `j/k select · Enter open · Esc back · g refresh` | `agent_scene.hpp` |
| N4 | Drillable row affordance: stronger `↳` / amber name (not only when selected) | projection labels in `rebuildViews` / `projectOneRow` |
| N5 | Micro-slide on enter/back: 2–4 frames horizontal offset on transcript body (reuse pageSlide pattern) | `agent_scene.hpp` draw + short-lived `drillSlide` state |
| N6 | `ensureSelectionVisible` already on selectDelta full rebuild — verify nested | `inkcell_app_model.hpp` |

## Out of scope

- Auto-enter on agent RESULT (explicitly rejected earlier)
- Full child timeline virtualization special-case
- Inspector pane redesign

## Animation policy

- **Showy-optional:** micro-slide is 2–4 frames only on route change, not continuous.
- Must not block input; slide is pure draw offset from `steady_clock` start.

## Acceptance

- [ ] Nested header never claims root agent name as “you are here”
- [ ] Nested status does not show root pending counts as if they were local (or labels them)
- [ ] Esc/back hint visible while nested
- [ ] Enter child / goBack has a short spatial cue
- [ ] Root performance unchanged (no per-token nested rebuild regression — refreshNested still gated)
- [ ] Existing drill tests still pass; add hint/breadcrumb assertion if cheap

## Implementation notes

- Prefer **vm fields** (`scopeName`, `nestedHints`, `metricsScope`) over scene special cases.
- Micro-slide state lives on ShellModel: `drillSlideStartMs`, `drillSlideDir` (±1).
