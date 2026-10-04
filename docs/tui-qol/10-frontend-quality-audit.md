# 10 — Frontend quality audit (DIY first pass)

**Status:** verified-first-pass, no sub-agent compute (operator directive: DIY).
**Method:** every row below was read in-tree this session with file:line evidence.
Sweep-debt: hub_draw/components/chat_body_views line-by-line pass still pending compute.
**Branch tip when audited:** `b4d3d8a` (perf) · `74381f5` (footer honesty) · `3687198` (inkcell modules-reorg compat).

---

## 0. Verified perf law (benched this session — `chat-draw-bench`)

| Cost | Number | Verdict |
|---|---|---|
| `estimateTokens` over 2.4MB history | 2.96 ms/call | **was the framerate killer** — footer paid it every frame; fixed in `b4d3d8a` (cache: 0.0001ms validate) |
| Idle chat draw, 120×40 | 0.38 ms/frame | healthy |
| Streaming draw (mutate+version bump), 6000 lines | 0.65 ms/frame | healthy — the draw pipeline was never the problem |
| Engine loop | 33ms tick, `skip_idle_draw(true)`, field throttled 10Hz (`inkcell_runtime.hpp:152-171`) | matches inkcell-perf laws |

Post-`b4d3d8a` the whole live frame budget is ~1ms. If lag persists in the field, next suspects: `model->drain(bridge)` per tick and the rebuild-views path per protocol event — both unmeasured. `chat-draw-bench` is in the tree (`make chat-draw-bench`) — extend it before theorizing.

---

## 1. Slop findings

| # | file:line | what | why it's slop | fix |
|---|---|---|---|---|
| S1 | `chat_footer.hpp:2` | header comment: "3 rows idle, 4th only while a turn is live" | false — footer is a fixed 5-row plate (the repo's own law) | rewrite header comment to the 5-row contract |
| S2 | `chat_view.hpp` (1306 lines) | one header carries: wrap-cache engine, transcript paint, ask-modal paint, help overlay, completion menu | 4 unrelated concerns; every chat touch recompiles all | peel: (a) wrap engine (`syncTranscriptWrapCache`/`materializeViewport`/span math ~640-830) → `transcript_wrap.hpp`; (b) ask paint (`drawAskDialog` ~1117) → `ask/paint.hpp` per tui-qol/09 S3; (c) completion menu (~1212) → prompt chrome |
| S3 | `agent_scene.hpp:760-910` + `899-1070` | footer-model builder + phase resolver inline in the scene | scene does model assembly + phase classification — 300 lines of non-scene logic | peel to `chat_footer_build.hpp` / `footer_phase.hpp`; scene calls them |
| S4 | `event_reducer.hpp:48` vs `dashboard_model.hpp:242` | two `nowMs()` helpers in two headers | duplicated utility, two clock sources to keep identical | one `src/ui/model/clock.hpp` |
| S5 | `chat_footer.hpp` (silent >= 8), `types.hpp:303` (45), `agent_scene.hpp` (fallback 45) | stall/silence constants inline and duplicated | one policy, three homes — drift waiting to happen | `kThinkSilentWarnSec`, `kStreamStallDefaultSec` in `types.hpp`, import elsewhere |
| S6 | `chat_view.hpp:1117+` `drawAskDialog` | own option/description string munging | duplicates `ask_dialog_model.hpp` `optionFromJson` shape logic — two parsers of the same JSON | render from `DialogCard` only; no JSON in paint |
| S7 | `src/tui/dialog.hpp` + `src/ui/chat/ask_dialog_model.hpp` | two dialog models (oracle vs product) | P0.2 from 2026-07-26 modularity audit — still open | oracle includes product model, or dies with `--tui legacy` |
| S8 | `chat_view.hpp:757` → `1010` | sync clears `blockKinds` → draw rebuilds full metadata every streaming sync | stable-prefix metadata thrown away per frame | rebuild metadata only for the dirty tail (mirror the span cache) |
| S9 | `agent_scene.hpp:995-1020` `chip()` | repeated `find()`-chain extracting path/command/pattern from action bodies | same extraction lives in `core/delegate/action.cpp` summary code | one `actionBodyDigest(name, body)` helper, both callers |
| S10 | `chat_view.hpp:83-87` | `ChatSurfaceModel` carries 4 raw writeback pointers (`selectionNavPending`, `scrollOffsetWriteback`, …) | draw mutates model state through pointers — paint has side effects | return a `DrawResult` struct; scene applies writebacks |

## 2. Architecture smells

| # | file:line | what | risk | fix |
|---|---|---|---|---|
| A1 | `agent_scene.hpp:866,893-899,903-904` (+`725,760,1166,1483,1502`) | UI thread reads live `Agent` state (`config()`, `history().size()`, `estimatedPromptTokens()`, `liveIteration()`) while the worker thread mutates the same objects mid-turn | **data race, crash-class** — `history_.push_back` can realloc during a UI read | publish an atomically-swapped `AgentStats` snapshot through the bridge (the footer already gets most fields via `vm`; move the rest there). Highest-priority item in this doc |
| A2 | `hub_draw.hpp` (1084 lines) | home/sessions/settings paint in one header — biggest unpeeled surface | same as S2 class | peel per page: `hub/home.hpp`, `hub/sessions.hpp`, `hub/settings.hpp` |
| A3 | `inkcell_app_model.hpp:80` | `ShellModel : TimelineStore` + composer + nav + dashboard + ask + palette + session fields | God-struct; also holds duplicate dashboard/nav state that `dashboard_model.hpp` also models | continue the F1-F5 peel: dashboard/nav state lives in one place only |
| A4 | `agent_scene.hpp:1207` | scene calls `model_->rootAgent->setProvider(...)` directly | scene drives agent lifecycle the app layer owns | route through `AgentBridge` action like other lifecycle ops |
| A5 | `components/workflow_*.hpp` (1183 lines total) | workflow canvas family | if unreachable from the hub it's a dead widget zoo — the exact anti-pattern the product skill bans | verify reachability; archive to `src/ui/_archive/` if not launch-reachable |

## 3. QOL gaps

| # | where | gap | operator impact |
|---|---|---|---|
| Q1 | `drawAskDialog` (chat_view.hpp:1117) | no scroll for long message/options — hard cutoff at `bottom()-6`/`-4` (chat-ux audit #28, still open) | long ask bodies get silently clipped mid-decision |
| Q2 | same | full-page modal steals the viewport; evidence behind it (the RESULT that prompted the question) is buried | decisions made without the evidence on screen — 09 S3 docks the well |
| Q3 | help overlay vs `handle_key` | keybind drift unverified (sweep-debt) | muscle memory + doc disagree silently |
| Q4 | compact/canvas views | wired to `timelineRows` but polish open (CONTINUATION #4) | operator index is functional, not finished |
| Q5 | transcript | no search/jump — see F1 below | long sessions are scroll-only |
| Q6 | `results` body truncation | hint exists (`· /truncate`) — verified good | none — keep |

## 4. Feature proposals (ROI-ranked, grounded in existing primitives)

| # | feature | grounded in | why it matters |
|---|---|---|---|
| F1 | **Transcript search**: `/pattern` → n/N jump across `TimelineRow`s (highlight + snap via `selectionNavPending`, which already exists) | TimelineStore rows, block reader, snap math | long sessions are unread without it; cheapest big win |
| F2 | **Frame HUD** (hidden keybind): frame ms, dirty rows, drain count per tick — inkcell `FrameStats` | inkcell FrameStats, footer plate | the operator debugs framerate by feel today; make it a number on screen |
| F3 | **Session quick-switch from chat**: palette action → sessions list → resume, no hub round-trip | Sessions OS store + cmd palette | kills the biggest navigation loop in daily use |
| F4 | **Steer affordance**: Ctrl-S composer steer mode + visible queued-steer send (footer already counts it) | steer plumbing + footer chip | steer is invisible UX today |
| F5 | **Per-kind filter in compact view**: kind chips (User/Tool/Result/Error) from `chat_blocks.hpp` palettes | chat_blocks kinds, compact rows | scan-to-answer in dense turns |
| F6 | **Expand truncated body** in block reader (`e` = full SoT view) | kBodyCap + block reader | the 8KB-mutilation class of surprise, ended |
| F7 | **Recent-command frecency** in palette | cmd palette | command recall without memory |
| F8 | **Ask result chips in transcript**: answered card renders as compact RESULT chip with values | ask_dialog results, timeline rows | ask answers vanish today after submit |

## 5. Contradictions with docs/plans

- `chat_footer.hpp:2` comment vs the fixed-5-row law (stale comment, S1).
- tui-qol/09 (docked ask well, magenta rail) vs shipped cyan Square full-page modal — planned-not-shipped, keep 09 as the authority.
- tui-qol/08 (Home launchpad/Sessions OS) vs current `hub_draw.hpp` — the three pre-existing `test-chat-scene` home FAILs ("home surfaces harness/runtime") are the drift marker; hub paint hasn't caught up to the plan.
- CONTINUATION checkpoint §"6-row footer" (2026-08-19) is stale — 5-row shipped.
- AGENDA §2 still lists inkcell migration as "remaining work" — cutover is done; the file predates the modules reorg.

---

## Execution order (recommendation)

1. **A1** (cross-thread snapshot) — crash-class, do first.
2. **S1/S4/S5** (comment + constants) — 20 minutes, kills drift.
3. **F2** (Frame HUD) — makes every future perf claim measurable on-screen.
4. **S2 peel** (chat_view) — unblocks 09 S3 ask paint and all chat work.
5. **09 S3 + Q1/Q2** (docked ask well) — the biggest operator-visible QOL.
6. **F1** (transcript search).
7. **A2** (hub peel) alongside the 08 hub overhaul — one motion, not two.

*GODSPEED.*
