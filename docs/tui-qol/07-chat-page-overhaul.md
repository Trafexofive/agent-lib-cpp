# Chat page overhaul — daily-driver plan

**Status:** plan only. No code until you pick a wave.  
**Date:** 2026-08-18  
**Operator:** you are the user. ROI = felt every session, not pi museum parity.  
**Supersedes as priority order:** `docs/AUDITS/REPORTS/2026-08-16-chat-ux-backlog.md` §C (items still valid; ranking below is live).  
**Does not replace:** `01-feel` / `02-composer` / `03-nested` packs — those are slices inside waves 2–3.

---

## 0. What “chat page” is (live)

Not a widget zoo. One scene + one draw surface + one composer.

```
AgentScene          1524 lines  keys, slash, drill, ask, submit  (god)
chat_view.hpp       1279 lines  header, transcript, prompt, help, ask paint
chat_footer.hpp      345        live / session / engine panes
block_reader.hpp     563        open-block inspect
shell_composer.hpp   121        history + submit
timeline + codec                 data, not paint
```

Contract that **must not change** (muscle memory):

| Key | Meaning |
|-----|---------|
| Enter | submit / slash |
| Shift+Enter / Ctrl+Enter | newline |
| Esc | composer → timeline → (not hub) |
| Ctrl-C | 1st cancel turn, 2nd idle quit |
| Ctrl-X | stop loop |
| Ctrl-U / K / W | kill bol / eol / word **when composer focused** |
| Ctrl-J | fine scroll down always |
| Ctrl-K | fine scroll **only** if timeline focused |
| j/k | block select in history focus |
| Tab | slash complete if stem starts `/` |

Home/End = **line motion in composer**, transcript jump in timeline. Tests now match this. Do not “fix” Home to jump transcript while typing.

---

## 1. Diagnosis (why it feels amateur / heavy)

Already shipped this week — **do not re-do:** Ctrl-U/K/W, muted Thought/Raw/Notice, empty-state copy, skip-idle live marks, exec hang-kill.

What’s still wrong **on the page itself:**

1. **`AgentScene` is the keymap, the slash router, and the turn controller.** Unreadable; every new bind is a landmine (`Ctrl-K` was the proof).
2. **`chat_view.hpp` paints five jobs** (chrome, transcript, prompt box, help overlay, ask cards). Tetris/aart split this: `views/` draw-only, `app/` keys.
3. **Composer is a TextArea trapped behind a 1500-line `on_key`.** `@` / `!cmd` / queue cannot land cleanly until submit/complete have a single function.
4. **Footer + header dump metrics the operator doesn’t scan** (token bytes, action counts) while missing the two things you *do* scan: **model/agent** and **is it running**.
5. **No steer queue.** Mid-turn you either wait or Ctrl-C. That’s the only *runtime* gap that changes how you work.
6. **Large results are a wall.** Ctrl-O truncate exists as a global toggle; no per-block collapse. You live in grep/exec output.

Non-problems: parser speed, curl on UI thread (worker), pretty session save (async), pi `/hotkeys` museum.

---

## 2. North star (one sentence)

**A chat page you can drive for a 2-hour coding session without thinking about the TUI** — readline composer, truthful running state, steer without cancel, results that don’t drown you, keys that never surprise.

Visual: graphite readable, one identity strip, prompt grows, transcript is blocks not a log dump. Not a dashboard glued under the prompt.

---

## 3. Waves (do in order)

### Wave 0 — extract, no behavior change (~2–3 h)

Goal: make Waves 1–2 *possible*. Tetris convention, not a rewrite.

- Split `AgentScene::on_key` into named handlers in `src/ui/scenes/chat_keys.hpp` (or `src/ui/chat/keys.hpp`): `handlePalette`, `handleGlobalCtrl`, `handleTimeline`, `handleComposer`.
- Split `chat_view.hpp` paint: `draw_header`, `draw_transcript`, `draw_prompt`, `draw_help` — still called from one `drawChatSurface`. **No new widgets in inkcell.**
- One function: `ComposerPolicy::submit(text) → { slash | prompt | empty }`. Today submit is scattered (trim, slash, pendingSubmit).
- **Deletions:** none of the product. Maybe dead comments. No `_archive` of the live scene.
- **Verify:** `test-chat-scene` `test-ui-model` identical behavior.

If this wave slips past 3h, stop. Don’t “improve” keys while extracting.

### Wave 1 — daily composer (~2 h)  ← **highest ROI after extract**

You type here 200×/day.

| ID | Change | Notes |
|----|--------|--------|
| C1 | Trailing-only trim on submit | Leading indent survives (code paste) |
| C3 | History draft restore | Up/Down already partial; finish |
| **@** | `@path` + Tab file complete | cwd-relative, no shell glob heroics |
| **!** | `!cmd` run in composer, `!!cmd` insert stdout | Uses `process::run` 30s cap; **never** hang. Output as a Notice/User hybrid row, not a fake agent turn |
| paste | If paste > N lines, status flash “N lines pasted” — **no confirm dialog** unless >200 lines | Confirm-every-paste is anti-ROI |

Out: message queue (Wave 2), bracketed-paste modal.

### Wave 2 — mid-turn steer (~3 h, touches runtime)

The one item that changes *how* you use the harness.

- Composer submit **while `running`**: enqueue text, don’t Ctrl-C.
- Drain into `Agent::getSteeringMessages()` (or equivalent) next iteration.
- UI: chip on status `queued 1` / `queued 2`; Esc on empty composer does **not** drop the queue (explicit `/stop` or Ctrl-X).
- Tests: submit-while-running does not set `pendingSubmit` as a new turn.

If runtime hook is missing, **stub the queue in the model** and inject at `runLoop` iteration boundary — don’t invent a second agent loop.

### Wave 3 — results you can survive (~2 h)

- Per-block collapse: selected result + `za` / Ctrl-O **on the block** (global truncate stays as `/truncate`).
- Body cap already 8–16 KiB — show `… N bytes` footer on the block when truncated (July #35, still real).
- Selection contrast: bump selected bg more than `+10` on graphite (P0-5). Cheap.

### Wave 4 — chrome honesty (~1–2 h)

- Header: **agent · model · running|ready** only. Session id in `?` help / footer pane, not the title.
- Help overlay: regenerate from the **actual** keymap after Wave 0 (today it’s a static lie).
- Nested drill: keep `03-nested-drill-pack.md` as-is; don’t mix into Wave 1.

### Explicitly not in this overhaul

- Ask-cards DAG polish (works; P2)
- `/hotkeys`, cache/cost footer
- Startup “loaded N tools” banner (noise)
- Multi-agent tab strip
- Promoting chat widgets into `include/inkcell`
- Async LLM / curl_multi
- Hub / field / pills (different page)

---

## 4. File map after Wave 0 (target)

```
src/ui/chat/
  keys.hpp           composer + timeline + global ctrl  (from agent_scene)
  submit.hpp         slash vs prompt vs bang vs at
  draw_transcript.hpp
  draw_prompt.hpp
  draw_help.hpp
  chat_view.hpp      orchestrates draws only
agent_scene.hpp      on_enter / draw / delegates on_key  (<400 lines target)
```

Do **not** mass-rename in one commit. One extract per commit, `test-chat-scene` green.

---

## 5. Verify bar (every wave)

```bash
make cortex-mk3 test-chat-scene test-ui-model -j$(nproc)
# live: graphite, empty chat, type, Ctrl-K kill, stream, submit-while-running (wave 2),
#       @README Tab, !true, long exec result collapse (wave 3)
```

No live API quota probes.

---

## 6. Pick

Recommended: **Wave 0 then Wave 1 in the same day** if extract stays dumb. Wave 2 the next sitting (runtime). Wave 3–4 whenever the page annoys you mid-session.

Say `wave 0` / `wave 1` / `0+1` to execute. Until then this file is the contract.
