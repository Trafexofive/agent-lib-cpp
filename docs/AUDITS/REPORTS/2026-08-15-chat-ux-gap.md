# Chat UX Gap — Cortex MK3 vs pi (visual & practical)

**Date:** 2026-08-15
**Status:** Evidence-backed gap assessment (companion to `2026-08-15-prompt-runtime-harness-audit.md`)
**Question:** "Visual and practical side we're still lacking vs pi — just the chat, basic stuff."

**Sources:** read from local node_modules (pi `0.84.1`) + MK3 `src/ui/chat/*`, `src/ui/scenes/agent_scene.hpp`, `docs/tui-qol/*`.

---

## 0. Verdict — the gap is real, but it's *polish*, not *absence*

MK3's chat is **far more built-out than the "we're lacking" framing implies.** The basics already exist: multi-line composer (Enter=submit / Shift+Enter=newline / Ctrl+Enter), prompt history with draft restore, fine transcript scroll (Ctrl-J/K), vim-ish block nav (j/k/gg/G), Pi-style Ctrl-C/Esc kill semantics, inline completion menu, streaming spinner + blink cursor, viewport-span virtualization, binary/UTF-8 sanitize.

What pi has that MK3 lacks is a **specific, enumerable set of UX conveniences** — mostly discoverability affordances and message-queue mechanics. None of it is a missing "basic chat." It's a polish gap on an already-functional surface.

The framing correction: **MK3 is behind pi on *convenience/discoverability*, not on *capability*.** Different problem, cheaper fix.

---

## 1. Where pi is genuinely ahead (the concrete list)

| # | pi feature | MK3 status | Gap class |
|---|---|---|---|
| 1 | `@` fuzzy file reference + Tab path completion | ❌ absent | **composer** |
| 2 | `!cmd` / `!!cmd` bash-from-composer | ❌ absent | **composer** |
| 3 | External editor (`Ctrl+G` → `$EDITOR`) | ❌ absent | **composer** |
| 4 | **Message queue** (Enter=steer, Alt+Enter=follow-up, Esc=abort, Alt+Up=retrieve) | ❌ absent (no steer/follow-up queue) | **runtime-interactive** |
| 5 | Clipboard image paste / drag-image | ❌ absent | **composer** |
| 6 | `/tree` jump-to-any-point + `/fork` + `/clone` + `/branch` rewind UI | 🔶 partial (Session hub has fork/title/delete; no in-context `/tree`/rewind) | **sessions** |
| 7 | Collapse tool output (`Ctrl+O`) / thinking (`Ctrl+T`) | 🔶 partial (`showThoughts`/`showRaw` flags exist; no per-block collapse) | **transcript** |
| 8 | One-keystroke model switch (`Ctrl+L` picker) | 🔶 partial (hub-level, not composer-global) | **chrome** |
| 9 | Context/cost/token **footer** (`↑ ↓ R W CH` + cost) | 🔶 partial (MK3 footer has tokenBytes, no cache-read/write or cost) | **chrome** |
| 10 | Startup header listing loaded AGENTS.md/skills/extensions | ❌ absent | **discoverability** |
| 11 | `/hotkeys` discoverable keymap | 🔶 partial (keymap exists, no `/hotkeys`-style surfacing) | **discoverability** |
| 12 | `/compact [prompt]`, `/session` info, `/export`, `/import`, `/share` | 🔶 partial (compaction is config-driven; no `/compact`; export exists) | **sessions** |

Legend: ❌ = not present; 🔶 = present but not parity.

**The two that matter most, day-to-day:**
- **`@` file reference + path completion (#1)** — the single biggest real-world editing-speed gap. Every pi session flows through it.
- **Message queue (#4)** — steering/follow-up while the agent is mid-run. This is *practical* (you stop waiting), not cosmetic.

Everything else is convenence that compounds but isn't blocking.

---

## 2. Where MK3 is *already* at-or-ahead (don't waste cycles)

- **Canvas/shaders/animation** — field shaders, pill slide, cmd-palette scale-fade, workflow pulse, braille spinner + blink cursor. pi has none of this; it's a plain-text terminal. MK3 is *ahead* on feel.
- **Vim-ish navigation** — j/k/gg/G + block selection + fine scroll is richer than pi's default scrolling.
- **Nested sub-agent drill** — MK3's core differentiator; pi has subagents but no equivalent in-place drill chrome.
- **Session hub** — fork/title/delete is present.
- **Sanitize / virtualization / projection** — shipped and not something pi notably beats.

**Don't spend effort "catching up" on these — MK3 already leads.**

---

## 3. The underlying reason (why it feels worse than it is)

Per `docs/tui-qol/00-inkcell-first-product-review.md`, MK3's chat is **functional but "works but amateur" in always-on surfaces** — chrome identity, graphite contrast/selection, composer paper-cuts, motion. The TUI rebuild on inkcell produced a *working* product with a *known polish backlog*, not a missing chat.

The real gaps vs pi split into three buckets:

1. **Composer richness** (`@`/`!`/external-editor/paste) — pure input-path features.
2. **Message-queue interactivity** (steer/follow-up while running) — a *runtime-interactive* feature, not visual.
3. **Discoverability** (startup header, `/hotkeys`, model picker, cache/cost footer) — chrome affordances.

Bucket 2 is the only one that touches the runtime/harness layer; buckets 1 and 3 are pure TUI.

---

## 4. Recommendation (ranked, scoped to "basic stuff")

### Tier 1 — closes the real daily gap, low effort

1. **`@` file reference + Tab path completion** in the composer. Highest-ROI single item. Pure `shell_composer`/`agent_scene` work.
2. **`!cmd` / `!!cmd` bash-from-composer.** Small, huge practical payoff for a systems operator.
3. **`/hotkeys` + startup header** listing loaded agents/skills/context — the discoverability tax that makes pi *feel* more capable than it is.

### Tier 2 — the practical interactivity gap

4. **Message queue** (steer vs follow-up). This is the one item that reaches into the runtime — needs `getSteeringMessages`-style plumbing, which the harness audit already flagged as parity-worth-having. Bigger lift, real payoff.

### Tier 3 — polish, already specced in `docs/tui-qol/*`

5. The existing `Feel` / `Composer` / `Nested` packs (chrome identity, contrast/selection, composer paper-cuts, breadcrumb truth). These are *already planned* — execute them, don't re-litigate.

---

## 5. Bottom line

"You think we're lacking the basic chat vs pi" → **partially true, mostly a framing error.** MK3 already ships a multi-line composer with history, vim nav, streaming, and richer *feel* than pi. What it lacks is a **specific set of composer conveniences (`@`/`!`) and a message-queue**, plus **discoverability chrome** — none of which is a missing "basic chat."

The cheapest, highest-leverage path to close the *felt* gap: **`@` file reference + `!cmd` + discoverability surface + the Feel pack.** Four items, no runtime changes except the message queue, all already mapped to files in `docs/tui-qol/06-item-catalog.md`.
