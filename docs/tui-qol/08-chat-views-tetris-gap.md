# Chat views + tetris gap — vision (not a ship list)

**Date:** 2026-08-19  
**Trigger:** live session looked hung at `acting · reading README.md` while **KB kept rising**. Not a freeze — **child agents still running**, footer lied. Tetris is the quality bar.  
**Law:** one `AgentScene`. Extra views = **opt-in modes**, not a second app. Composer owns Enter. Product chrome stays in Cortex.

---

## 0. What just happened (UX bug, not “hang”)

Footer phase is **last-12-rows scan**. Last *tool* was `fs_read README` → chip stuck on “reading README.md” while `ActionType::AGENT` children filled the transcript (tokenBytes ↑). Operator Ctrl-C’d a live system.

**Cheap truth (shipped this sitting):** if newest Action is `actionType == "agent"` → phase `delegate` / “waiting on child · {name}”. Synonym rotates. `pendingOps` already exists — still unused in the live line.

Still missing: **how many children, who, how long**. That’s a *view*, not a synonym.

---

## 1. What tetris does that Cortex chat does not

Tetris (`libs/inkcell/examples/inkcell-tetris`): cabinet, not a clone skin.

| Tetris | Cortex chat | Gap |
|--------|-------------|-----|
| `app/` keys · `views/` draw · `data/` state | `agent_scene` 1500 lines owns keys+draw+phase | **god scene** |
| Named **pages**: Attract / Modes / Play / Settings / Stats / GameOver | One dump + Ctrl-F footer 3-pane | no **named modes** |
| Overlay stack (binds / mods / help) eats keys, Esc peels | Help overlay + ask + reader, Esc ladder is a novel | overlays exist; not a **stack** |
| Header: **wordmark + mode + score** — one identity strip | No header (`drawHeader` is **dead**). Identity dumped in footer row 2 | **no face** |
| Field is leftover cells, cluster **floats** | Field is wallpaper under everything; chrome fights it | chrome not designed *on* the field |
| HUD is **wells** (HOLD / NEXT / heat), not a metric sentence | Footer is one 80-col sentence: phase · actN · resN · KB · iter | **telemetry essay** |
| `?` is **page help** for *this* page | `?` is registry dump | help isn’t contextual |
| Nav lock 70ms — no key chatter | none | cheap feel |
| Attract vs Play — idle has a *mode* | idle = empty transcript + “ready” | empty does not invite |

Steal **feel**, not the well. Chat is a protocol instrument, not a game.

---

## 2. Ambition — chat as a **cabinet of views**

Same scene. `v` (or grow `Ctrl-F`) cycles **view**. Composer always on the bottom unless a modal owns keys.

```
┌ identity (agent · model · RUN|READY) ──────────────────────────┐
│ VIEW: transcript | protocol | fleet | context | pins | files   │
│                                                                │
│   (active view body)                                           │
│                                                                │
├ composer ──────────────────────────────────────────────────────┤
│ footer: TRUTH line (one verb) + pressure, not a log            │
└────────────────────────────────────────────────────────────────┘
```

### Views (rank after your next live hour — delete what you didn’t miss)

1. **Transcript (default)** — blocks, collapse, follow-edge. What you have.
2. **Fleet / DAG** — in-flight tools + **subagents as wells**. Name, elapsed, last line. This is the “I thought it hung” view. Tetris HOLD/NEXT energy.
3. **Protocol** — CANON stream beside (or instead of) pretty blocks. Split when wide; replace when narrow. `/raw` is a toggle, not a view.
4. **Context** — token bar (you have it) + **what compaction will eat** + pins. Recitation surface (Manus). Pairs with next-cycle harness work.
5. **Pins** — operator `never_drop` facts. Empty invites `:pin`.
6. **Files** — `@` hits + last `fs_read` peek. Don’t rebuild aart.
7. **Spine** — turn index (YOU rows). Jump without scrolling 600 lines.

Zen: views **replace** the body, they don’t stack chrome. Footer stays 2–3 rows. Identity strip is **one** row at top (resurrect `drawHeader` for this, kill the footer identity dump).

---

## 3. Truth line (non-negotiable)

One verb, one object, one clock:

```
⠼ waiting on discovery  ·  1m24s  ·  2 children  ·  3 tools
```

Not: `acting · reading README.md  ·  act23 · res20 · 404.7KB  ·  23/1800`.

KB / ctx / hist belong on **Context** view or a dim second row — not the live verb.

Phase machine (honest):

| State | Verb |
|-------|------|
| TTFT, no protocol yet | waiting on model |
| thought | thinking |
| tool | reading X / exec Y |
| **open agent action, no result** | **waiting on {child}** |
| N children | waiting on N specialists |
| response | composing |
| ask | waiting for you |
| idle | ready |

If `pendingOps > 0` and last painted tool already has a RESULT, **do not** keep that tool’s verb.

---

## 4. Implementation order (when we actually code)

| Slice | Why |
|-------|-----|
| A. Honest phase (agent/delegate) | you just lived this |
| B. Identity strip + footer diet | tetris header vs essay |
| C. Fleet view (`v`) | only new *view* that pays immediately |
| D. Context view | harness cycle |
| E. Protocol split | power user |
| F. Pins / files / spine | after you miss them |

Do **not** extract `agent_scene` until C needs a second draw function. Then `views/chat_transcript.hpp` + `views/chat_fleet.hpp`.

Catalog: **nothing** here belongs in `include/inkcell` until calendar wants the same cabinet.

---

## 5. After compact

Load this file + `07-chat-page-overhaul.md` + quality-day checkpoint. Next coding cycle is still **prompt/harness/subagent** unless you say “ship fleet view first.”
