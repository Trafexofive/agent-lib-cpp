# Chat UX Backlog — Reconciled (July audit → current code)

**Date:** 2026-08-16
**Purpose:** Merge the 50-item July audit (`2026-07-28-chat-ux-audit.md`) and the pi-parity gap (`2026-08-15-chat-ux-gap.md`) into ONE triaged, current-state backlog. Each item is re-verified against the *current* source — the chat surface was rewritten 08-10 (`d53a35e` "chat surface — block reader, footer, timeline codec"), so July's exact `file:line` refs are stale but the *items* were re-checked.

**Status key:** ✅ SHIPPED (confirmed in current source) · 🔶 PARTIAL · ❌ OPEN · 🆕 new (pi-parity, not in July audit)

---

## A. Already shipped (re-verified — stop tracking these)

| July # | Item | Evidence in current source |
|---|---|---|
| 10 | Multi-line composer (Enter=submit / Shift+Enter=newline / Ctrl+Enter) | `agent_scene.hpp:600-615` composer contract |
| 19 | Breadcrumb / back affordance in header | `chat_view.hpp:186-187` `◀ scopeName`; `agent_scene.hpp:641` `vm.path=breadcrumb()` |
| 23 | Back hint / drill transition | `scopeName` accent in status bar |
| 31 | `secret` hides input | `chat_view.hpp:1170` `input.size()*'*'` |
| 44 | Viewport virtualization | `chat_view.hpp` span-based `wrapTranscriptRange` + spans |
| — | Sanitize, PI Ctrl-C/Esc, spinner, vim nav, block reader, footer | shipped in 08-10 rewrite |
| — | Header agent identity / grouping (F1/F2) | `ChatSurfaceModel.agentName`/`scopeName` wired |

---

## B. Still OPEN — real work remaining (triaged, current-state)

### P0 — visible every session, "amateur" feel

| ID | Item | Current status | Where |
|---|---|---|---|
| P0-1 | **Ctrl-U / Ctrl-K / Ctrl-W** line-editing | ❌ still absent; Ctrl-K is *stolen* by fine-scroll (`agent_scene.hpp:401`) | `shell_composer` / `agent_scene.hpp` |
| P0-2 | **Graphite dim contrast** (July #40) | ❌ `dim()` = `rgb(110,110,118)` + `s.dim=true` — the exact 3.x:1 failure remains; `muted()` (rgb 140) exists but isn't applied to chrome | `cortex_theme.hpp` |
| P0-3 | **Thought/Raw/Notice contrast** (July #41) | ❌ still dim-fg-on-dark for these kinds | `chat_blocks.hpp` |
| P0-4 | **Empty-state message** (July #15) | ❌ `logical.empty()` pushes a blank row (`chat_view.hpp:240`), no "Type a prompt" hint | `chat_view.hpp` |
| P0-5 | **Selection highlight strength** (July #37/42) | 🔶 `+10/+12` boost; subtle on graphite | `chat_blocks.hpp` |

### P1 — daily friction / discoverability

| ID | Item | Current status |
|---|---|---|
| P1-1 | 🆕 **`@` file reference + Tab path completion** (pi parity #1) | ❌ absent — highest-ROI pi gap |
| P1-2 | 🆕 **`!cmd` / `!!cmd` bash-from-composer** (pi parity) | ❌ absent |
| P1-3 | 🆕 **Message queue** (steer vs follow-up) (pi parity) | ❌ absent — touches runtime (`getSteeringMessages`) |
| P1-4 | **Collapsible large results** (July #43, Ctrl+O) | ❌ absent |
| P1-5 | **Trailing-only trim** on submit (July #8) | ❌ confirm; `submitComposer` leading-trim behavior |
| P1-6 | **Help overlay refresh** (July #25-27) — grouped/context-aware/theme | 🔶 static list still at `chat_view.hpp:1038` |
| P1-7 | **Bracketed-paste confirm** (July #12) | ❌ absent |
| P1-8 | 🆕 **Startup header** listing loaded agents/skills/context | ❌ absent |

### P2 — polish / power-user

| ID | Item |
|---|---|
| P2-1 | Ask-dialog scroll, type-colored borders, selection count (July #28-32) |
| P2-2 | Notice/error wrapping, `RAW N bytes hidden` placeholder (July #33-36) |
| P2-3 | Status `block N/M`, auto-scroll-on-select (July #38-39) |
| P2-4 | History cap/dedup (July #49), `/cp` toast feedback (July #47) |
| P2-5 | 🆕 **`/hotkeys` + cache/cost footer** (`↑↓ R W CH`) — discoverability (pi parity) |

---

## C. The reconciled "what to actually do next" (ranked)

1. **P0-1 (Ctrl-U/K/W)** — composer line-editing. ~30 min, daily friction, already had a branch attempt (`fix(prompt-history)` adjacent).

2. **P0-2 + P0-3 (graphite contrast)** — swap `dim()`→`muted()` for chrome + Thought/Raw/Notice to `text()`. Small, makes it stop looking broken on graphite.

3. **P1-1 (`@` file reference) + P1-2 (`!cmd`)** — the two pi-parity items that create the biggest *felt* gap. Pure composer-path work, no runtime change.

4. **P0-4 (empty state) + P0-5 (selection contrast)** — cheap completeness.

5. **P1-3 (message queue)** — the one runtime-reaching item; needs `getSteeringMessages` plumbing (already flagged in the harness audit).

Everything else is P2 polish that can batch into a later "QoL day" (same scope as the existing `docs/tui-qol/04-full-qol-day.md`).

---

## D. Housekeeping notes for this reconciliation

- **`cortex_theme.hpp` was never touched post-07-28** — the contrast items (40/41/42) were never attempted, despite being P0. `muted()` helper already exists and is the intended fix path; it just needs to be *applied*.
- **July line refs are stale** — `chat_view.hpp` grew from ~480 → ~1260 lines. Any future work must re-locate symbols; this doc's "where" column uses current anchors, not the July audit's.
- **`README` cross-link**: the three docs (`07-28` audit, `08-15` harness audit, `08-15` chat-gap) + this backlog should live in `docs/tui-qol/` or `docs/AUDITS/REPORTS/` under a single index — deduped so no future agent re-derives this.

---

*Reconciliation done — no code modified. This supersedes the July audit's prioritization; the July audit remains the historical record.*
