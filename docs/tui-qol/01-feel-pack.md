# Feel Pack (recommended first)

**Goal:** Make the always-on chrome read as intentional product, not POC.  
**Effort:** ~2–3 hours.  
**Risk:** Low — draw/theme only + tiny model wiring.  
**Audit refs:** #1, #3–5, #17, #37, #40–42 + light motion.

## Problem (what you feel every turn)

1. Header still leads with a generic title (`CORTEX MK3` default) while `agentName` exists.
2. Graphite **dim** fails readable contrast on panel bg; Thought/Raw/Notice look “broken”.
3. Selection highlight is a +10 RGB nudge — invisible on graphite.
4. Running state has a spinner but the footer accent is static — no “alive” pulse.
5. Hub/chat notices appear/disappear with no fade — cheap.

## In scope

| # | Change | Primary files |
|---|--------|----------------|
| F1 | Header left identity = `agentName` (fallback title); keep path breadcrumb styling | `chat_view.hpp` `drawHeader`, `agent_scene.hpp` vm fill |
| F2 | Idle status: show last turn elapsed + bytes when not running | `chat_view.hpp` `drawStatusLine` |
| F3 | Raise graphite `dim()` luminance; Thought/Raw/Notice use readable fg (no triple-dim) | `cortex_theme.hpp`, `chat_blocks.hpp` |
| F4 | Selection boost +25–30 or panel_3; optional soft pulse when `historyFocused` | `chat_blocks.hpp`, `chat_view.hpp` |
| F5 | Running accent pulse (phase from `nowMs`, ~1–1.5 Hz, subtle) | `chat_view.hpp` status/prompt accent |
| F6 | Toast fade for notices (hub `dashboard.notice` + chat notification top) — 200–300ms ease | `main_scene.hpp` or small `toast.hpp`, `notification.hpp` |
| F7 | Mode string polish already partial (`think · trunc · neon`); ensure help/`?` matches | `agent_scene.hpp`, `drawHelpOverlay` |

## Out of scope

- Composer Ctrl bindings (composer pack)
- Nested metrics truth (nested pack)
- Multi-line input, bracketed paste confirm
- New shaders / field variants

## Animation policy (this pack)

- **Subtle only:** pulse on live accent + toast opacity; no constant full-screen motion.
- Reuse `nowMs` / `gfx::nowSeconds()` — no new timers.
- Cap extra draw work: pulse is style only (no full surface re-layout).

## Acceptance

- [ ] Graphite: Thought body readable at a glance
- [ ] Selected block obvious without squinting
- [ ] Header shows real agent name at root; path still shows nested scope
- [ ] Idle footer shows last elapsed when a turn completed
- [ ] Running: accent ticks without eye strain
- [ ] Notice appears → soft fade out (or dim hold) then clear
- [ ] `test-chat-scene` + `test-ui-view` green; neon still looks intentional

## Implementation notes

- Prefer **theme tokens** over one-off RGB in scenes.
- Selection pulse: modulate only when `historyFocused && !running` to avoid fighting stream redraw.
- Toast: store `noticeMs` / `noticeBornMs` on dashboard; draw alpha via dim levels if Surface has no true alpha (glyph/style ladder is fine).
