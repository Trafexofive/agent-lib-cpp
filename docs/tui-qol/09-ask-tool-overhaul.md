# 09 — ask_tool overhaul

**Status:** PLAN — not executed  
**Bar:** operator-grade human I/O. Not a pi clone. Not a modal that steals the chat.  
**Law:** the model never guesses when the operator can answer in one card. The harness never hangs the ReAct loop on a dialog.

---

## 0. What it is

`ask_tool` is the **only legal operator I/O channel** inside a live turn.

Not STEER (mid-turn guidance, non-blocking).  
Not the composer (next user prompt).  
Not a TUI gimmick.

It is a **tool**: `<action type="tool" name="ask_tool">` → structured `results` map → `<result>` in history. Same contract as `list` / `grep`. If the TUI dies, the tool still has a defined outcome (`timed_out` / `cancelled` / `no_tty`).

Pi's `ask_cards` is a **dialog DAG engine** with branches, conditions, loops, optionsResolver, dryRun. Cortex has a **linear card tape** plus a schema that *names* those types. That lie is the overhaul.

---

## 1. Current map (honest)

```
LLM JSON
  → normalizeAskParams          src/tools/ask_protocol.hpp     (~70)
  → Agent::dispatchAskTool      src/core/agent_tool_dispatch.cpp
       ├─ askToolHandler_       inkcell: AgentBridge::requestAsk
       │     CV wait 120s on the AGENT WORKER
       │     UiEvent::AskDialog
       │     parseDialogState   src/ui/chat/ask_dialog_model.hpp (~380)
       │     drawAskDialog      src/ui/chat/chat_view.hpp
       │     keys → advanceDialog / completeAsk / cancelAsk
       ├─ TTY stdin fallback    src/tools/builtins/ask_tool.cpp (~132)
       └─ no TTY                error, no hang
  → askResult {success, cancelled, timed_out, results, answered, count}
```

| Piece | Lines | Owns | Does not own |
|---|---|---|---|
| `ask_protocol.hpp` | 72 | wire in/out | types, DAG, TUI |
| `ask_tool.cpp` | 132 | stdin linear walk | TUI, timeout, secrets |
| `ask_dialog_model.hpp` | 379 | card parse + validate + linear advance | draw, keys, DAG |
| `agent_bridge.hpp` `requestAsk` | ~80 | CV + 120s wall | idle timeout, queue |
| `drawAskDialog` | chat_view | paint | scroll (audit: hard cutoff) |
| `tool.yml` | schema | names 13 types | engine for DAG |
| `ask_tool_test.cpp` | 73 | stdin + legacy normalize | TUI keys, timeout, cancel, DAG |

**Live (HOWTO, still true):** modal overlay, y/n, j/k choice, Space multi, type_confirm, secret mask, default Enter, Esc cancel, notes auto-advance, 120s wall timeout, headless fails loud.

**Named in schema / HOWTO as “later”:** nested chains, `branches`, `goto`, `condition`, `exitIf`, `loop`, `optionsResolver`, `dryRun`, `minDelaySecs`, `command_approval`.

---

## 2. Failure modes (the mountain)

Ranked by damage, not by how pretty the modal is.

### P0 — correctness / hang class

1. **Worker CV block inside `parser.feed()`.** `ask_tool` is SYNC. The generate that emitted it is still open *or* the next generate is waiting. Same class as leftover thinking: the loop is parked. Sibling tools in the same batch cannot finish painting. Footer `recovering · 2 open` while a dialog sits is this.

2. **Schema lies.** `tool.yml` advertises `ranker`, `key_value`, `multi_choice`, `textarea`, `secret`. Model emits them. Engine: linear tape, unknown → `text`. Ranker “Enter accepts 1..n” is a HOWTO sentence, not a widget. The model thinks it asked a DAG; it got a tape.

3. **Secrets in history.** `type=secret` is masked in the TUI. `<result>` and `raw.md` / `iterations.md` still get the value. Dumps are public to the next generate.

4. **Timeout is wall-clock from emit, not idle.** Operator reading a 40-line `message` gets `timed_out` at 120s while still deciding. Default is too short for “rate this plan.” Too long for a y/n that the operator walked away from.

5. **One global ask slot.** Second `ask_tool` while the first is live: undefined (bridge `askPending_` is a bool). Nested agent that also asks: stomps.

### P1 — product / plate

6. **Modal steals the viewport.** Operator cannot scroll the evidence that *caused* the question. Pi puts the card *in* the conversation. Cortex overlays.

7. **No scroll on long `message` / options** (chat-ux audit #28). Hard cutoff at `frame.bottom()-5`.

8. **Footer phase.** Dialog live should be `ask`, not `recovering` / `thinking`. Occupancy should show `ask 1/3` (card index), not `open 3` of leftover tools.

9. **Cancel vs Ctrl-X.** Esc on the dialog must cancel **the tool**, not the turn. Today `cancelAsk` unblocks with `cancelled=true`. Confirm that Ctrl-X during ask is turn-cancel, Esc is tool-cancel. Two keys, two outcomes, one sentence in the harness.

10. **Headless / HTTP (BT11).** Server path still “no TTY.” REST consumers importing `ask_tool` hang-or-error. Need a non-TTY contract: return `needs_human` without blocking, or a callback URL. Not this week unless you say so.

### P2 — engine (pi parity, Cortex-shaped)

11. No `condition` / `goto` / `branches` — cannot skip a confirm when the previous choice was “no.”
12. No `optionsResolver` — cannot list live sessions / models / agents.
13. No `command_approval` — destructive shell is still “type the word” not “edit the command.”
14. No `dryRun` — cannot unit-test a chain without a TTY.
15. `minDelaySecs` missing on `type_confirm` — anti-reflex.
16. Ask-tool **schema novels** in every system prompt (dump 184KB / 3 iters). Diet: cards yes, five essays of examples no.

### P3 — hygiene

17. Dual dialog models (`src/tui/dialog.hpp` vs `ui/chat/ask_dialog_model.hpp`) if the oracle still compiles. One header.
18. Stdin fallback is a second engine. Must consume the **same** `AskEngine` as TUI.
19. Tests do not fail when TUI keys regress. Stdin mock is not the product.

---

## 3. Architecture (the overhaul)

Three modules. No fourth.

```
src/tools/ask/
  protocol.hpp     // already exists as ask_protocol.hpp — keep the wire
  engine.hpp/.cpp  // DAG: cards, cursor, condition, goto, branches, loop
  cards.hpp        // card types as data + validate() + apply(answer)
  resolve.hpp      // optionsResolver registry (tool name → string[])

src/ui/chat/ask/
  paint.hpp        // draw — scroll, evidence strip, not a stolen full page
  keys.hpp         // keymap per type
  model.hpp        // thin: engine snapshot + input buffer + highlight

src/core/ + bridge
  dispatchAskTool  // does not CV-wait on the generate thread
  AskSession       // per-turn, queueable, id'd; not a process-global bool
```

**Wire stays.** `normalizeAskParams` + `askResult(...)` are law. Do not rename keys. Do not mint a second JSON shape for TUI.

**Engine is headless.** `AskEngine::feed(answer) → {next card | done | error}`. Stdin, TUI, tests, HTTP all call this. If TUI keys and stdin disagree, the engine test is the judge.

**Ask is async-to-the-loop, sync-to-the-tool.** The *tool* still blocks until the operator answers (that's the contract). The *generate* that produced the action must already have returned (generation-cut / batch settle). Implementation: `ask_tool` is still `mode=sync` for the model, but dispatch **must not** run inside `parser.feed()`. Park the action, finish the generate, then run asks. Same rule as “don't execute tools on the SSE callback” — if we already peeled that, ask is the remaining tenant.

**Not a pi clone.** Cortex does **not** need `command_approval` as a shell editor on day one. Cortex **does** need: evidence-visible, secrets redacted, DAG skip, one engine, tests that fail.

---

## 4. Visual — paint spec (this is product, not chrome)

### 4.0 Crime scene (today)

`drawAskDialog` (`chat_view.hpp` ~1117):

- Centered **modal** on the **page**, `min(page.w-4, 92) × min(page.h-4, 24)`.
- `surface.fill(panel_2)` + **cyan square box**. Ask is already `ChatBlockKind::ToolAsk` **magenta**. The modal ignores that and paints like a help overlay.
- `wrap_words(message)` then **hard cut** at `frame.bottom()-6`. No scroll. 40-line why-text dies.
- Options: `> ` marker, no wash, clip at `bottom()-4`. 20 options → first ~14, rest gone, no `↑↓` hint on the list itself (hint is a footer line).
- Input is `> …█` **inside the box**, so the real composer is dead and visually orphaned.
- `agent_scene` draws it **on top of** `drawChatSurface` after the 5-row footer is already painted. Evidence is under a slab.
- Help overlay uses the same “steal the page” trick. Ask is not help.

If it looks like a settings dialog, it is wrong.

### 4.1 Law

1. **Evidence stays.** Transcript remains the body. Ask is a **docked well**, not a modal.
2. **Magenta, not cyan.** ToolAsk palette already exists (`chat_blocks.hpp`). Rail + header use that. Cyan is protocol/assistant. Mixing them is why the box feels like a foreign app.
3. **Composer is the input.** Text/secret/number/type_confirm type into the existing prompt box (masked for secret). Choice/confirm **do not** steal typing — they use j/k/y/n. The prompt box restyles (magenta caret, placeholder = card title) while ask is live. Do not draw a second `> █` inside the well.
4. **12-row floor.** Well height = header(2) + message(wrap, cap 8) + widget + hint(1). Terminal `h < 18`: collapse message to 2 lines + `…`. Never cover the last RESULT card entirely on `h ≥ 24`.
5. **No sin(), no spinner, no pulse.** Idle seconds are a **number**. Urgency is color, not motion (`inkcell-perf`).
6. **Graphite first.** Neon is the same layout, louder magenta. No second geometry.

### 4.2 Layout (chat column, bottom-up — same as footer/prompt)

```
┌ transcript (scrollable) ────────────────────────────────────────────┐
│  ▎ RESULT  list  #l1  ·  0ms · 167B                              │
│     dir  tools · dir  core · …                                     │
│  ▎ TOOL  ask_tool  #ask1  sync                                     │
└─ ask well (docked) ─────────────────────────────────────────────┘
  ▎ ASK  2/4  choice · 48s
  │  Pick specialist
  │  Reader already listed src/core. Tester idle.
  │  █ reader                         ← wash = selected_style + magenta rail
  │    tester
  │    reviewer                       ← 3 more · j/k
  └  j/k  ↵ choose  ·  esc cancels this tool, not the turn
┌ prompt (restyled) ───────────────────────────────────────────────┐
│ ▎ ask · worker · type to filter choices                           │
└ footer 5-row plate (unchanged height) ──────────────────────────────┘
  row0  discovery  opencode-go/ox-alpha-free              idle 48s
  row1  ask · card 2/4 · choice · #ask1                   stream
  row2  ctx bar
  row3  turn n/cap  open 0  hist …                         (no recovering)
  row4  agent · agent
```

**Dock:** well sits **above the prompt**, **below the transcript**. `chatFooterReserve` stays 5. New: `askWellHeight(state, cols)` in the same bottom-up math as `promptBoxHeight`. Transcript `viewport_h` shrinks by well height. Stick-bottom still works — last RESULT stays parked against the well, not under it.

**Not a second composer.** Prompt row exists. Well is the question. Prompt is the answer (or a filter for long choice lists).

### 4.3 Chrome tokens

Reuse `KindPalette` ToolAsk. Do not invent a fourth magenta.

| Element | Graphite | Neon | Token |
|---|---|---|---|
| left rail `ASK` | rgb(190,110,185) | rgb(240,120,230) | `kindPalette(ToolAsk).rail` |
| well wash | rgb(34,22,36) | rgb(36,8,42) | `wash` |
| header fg | rgb(220,160,210) | rgb(250,150,240) | `head` |
| selected option wash | panel_3 + rail | same | `selected_style` **plus** magenta rail, not green selected |
| type_confirm word | amber | amber | `theme::amber()` — only this type gets amber |
| error | red | red | `theme::red()` one line, not a banner |
| secret bullets | dim | dim | `*` of display-width 1 |
| idle ≥15s | dim | dim | |
| idle ≤ 15s | amber | amber | |
| idle ≤ 5s | red | red | still no blink |

Border: **no box**. Rail `▎` / `▌` like TOOL/RESULT cards. A square cyan box is the modal. Kill it.

Progress: `2/4` as text. Optional 4-cell ticks `●●○○` in dim/magenta — only if it fits in the header without wrapping. No rainbow bar.

### 4.4 Per-type widgets (one well, different guts)

**Header (all types), 2 rows:**

```
▎ ASK  2/4  choice · 48s
  Pick specialist                  ← card.title, bright, one line, truncate
```

Chain title (`state.chainTitle`) is **not** a second H1. If it differs from card.title, it is the dim suffix: `ASK  2/4  ·  New project`.

**Body (`message`)**, wrap, cap 8 lines, `askMsgScroll` for overflow. `↓ n` in dim on the last visible line when clipped. j/k in **choice** does not steal this scroll — `shift-j/k` or `ctrl-d/u` for message. Default: message is short; if the model wrote a novel, scroll exists.

**choice** — vertical list, one option per row.

```
  █ reader          ← selected: full-width wash, rail on the well already
    tester
    reviewer
```

- `j/k` `↑↓` move. Wrap optional: **no wrap** (stay at ends) — accidental wrap on a 2-option list is a misclick.
- Type-to-filter in the **prompt** (fuzzy, case-insensitive). Filter line: prompt placeholder `filter choices`. Empty filter = all.
- Disabled options: dim, skip in j/k.
- Description (`option.description`): same row, dim, after two spaces, truncate. Never wrap a second line per option (that's how 20 options become 40).
- Viewport: well shows `min(n, 8)` options; selected always in view (`optionScroll`). Header `+12` when overflow.

**multi_choice** — same list, `[x]` / `[ ]` after the rail, Space toggles, Enter submits. Count in header: `3 selected`.

**ranker** — list with index `1.` `2.` …; `shift-j/k` reorder (or `ctrl-j/k` if shift is stolen). Enter accepts order. Prompt can still take `3,1,2`. **v1 widget can be list+typed order**; do not ship a broken drag fantasy in a TUI.

**confirm** — no list. Two pills on one row:

```
  [ Y yes ]    n no
```

Selected pill = wash + bold. `y`/`n` fire immediately (keep). `h/l` move highlight; Enter fires highlight. Default highlight: **No** for `urgency` high/critical (when we add it), else Yes. Until urgency exists: default **No** if title/message contains `delete`/`drop`/`deploy`/`overwrite` (cheap, honest). Document the heuristic; don't get cute.

**type_confirm** — no pills. Amber word, remaining delay.

```
  type  DEPLOY
  wait 1.2s                    ← only if minDelaySecs > 0, then vanish
```

Input is the **prompt**, masked? No — this is a visible word. Prompt placeholder = `type DEPLOY`. Mismatch: error line in well, do not advance. Match: submit.

**text / textarea / number** — well is question only. Prompt is the field. Number bounds as dim subline in the well (`≥1  ≤65535`). Textarea: prompt wrap already caps 8 — keep it; do not invent a nested editor.

**secret** — well says `secret · not written to history`. Prompt shows `*` × length. Clipboard paste allowed. Error if required && empty.

**key_value** — well lists current pairs as `k = v` rows; prompt accepts `key=value` or `key=` to delete. v1 can be “type pairs, Enter submits all”. Don’t ship an inline table editor.

**note / info / section_header** — no widget, 1s auto-advance (keep), well shows the text, hint `(auto)`.

### 4.5 Motion, focus, keys

| Key | Ask live |
|---|---|
| Esc | cancel **tool** (`cancelled=true`). Turn stays. Composer returns to normal. |
| Ctrl-X | cancel **turn** (existing). Dialog dies with the turn. |
| Ctrl-C | same as Esc on the dialog if ask focused; process SIGINT still g_hardKill |
| Enter | submit current card (choice/confirm/text) |
| y / n | confirm only |
| j/k ↑↓ | choice list / confirm pills |
| Space | multi toggle |
| Tab | no-op v1 (do not cycle hidden fields) |
| Ctrl-O | still stream/compact **behind** the well. Well stays. |
| Page keys | transcript scroll **behind** — evidence. Well does not move. |

Focus: there is no focus ring circus. If the card is choice, keys go to the list; printable chars go to prompt-as-filter. If the card is text, all printable goes to prompt. Status line in the well hint **always** says which.

### 4.6 After submit (timeline, not a toast)

The live well **unmounts**. In the transcript, the existing TOOL ask_tool card stays. RESULT paints:

```
▎ RESULT  ask_tool  #ask1  ·  12s ·  ok
    worker: reader
```

Cancel:

```
▎ RESULT  ask_tool  #ask1  ·  cancelled
    (operator esc)
```

Timeout: `timed_out · idle 60s` in amber, not red (red is protocol_error).

Secret results: `token: ***`.

Do not leave a ghost well. Do not flash a green “thanks.”

### 4.7 Tight terminals

| rows | well |
|---|---|
| ≥ 28 | full: message cap 8, options cap 8 |
| 20–27 | message cap 3, options cap 5 |
| 16–19 | message 1 line truncate, options cap 3 |
| < 16 | header + widget only; message in prompt placeholder |

Width < 60: drop `· 48s` to the footer (already there); well header is `ASK 2/4 choice`.

### 4.8 Snapshot tests (visual, no TTY)

`ui_view_test` already calls `drawAskDialog`. Extend:

| Fixture | Assert |
|---|---|
| choice 3 opts, 96×26 | no `┌` box drawing chars; magenta rail present; `reader` on selected row |
| message 40 lines, 96×24 | last visible message line has `↓`; RESULT mock above still painted |
| 20 options, selected=19 | option 19 visible; `+n` overflow |
| secret input `abc` | well has no `abc`; prompt cells are `***` |
| confirm | `Y` / `n` pills; no second `> █` in well |
| 16×60 | well height ≤ 7; footer still 5 |

Fail the test if the paint uses `BorderStyle::Square`.

Timeout: **idle** clock, reset on any key. Default **300s** for chains with `message` longer than 200 chars, **60s** for confirm/choice with no body. Override `timeout_sec` still wins. `0` = no timeout only if the operator set it (never the default).

---

## 5. Engine contract (minimal DAG)

Implement these, in this order. Everything else is sugar.

| Feature | Meaning | Slice |
|---|---|---|
| linear tape | what we have | 0 (keep) |
| `required: false` skip on empty | already half-there | 1 |
| `condition` skip | `{cardId, op, value}` | 2 |
| `goto` / `branches` | jump after answer | 2 |
| `exitIf` | end chain early | 2 |
| `note`/`info` auto | have | 0 |
| `optionsResolver` | named registry fn | 3 |
| `loop` | N times, cap 50 | 4 |
| nested `chain` | sub-engine | 4 |
| `dryRun` | return plan, no TUI | 2 |
| `minDelaySecs` | type_confirm | 1 |
| secret redaction | result + history | 1 |

**Do not implement in v1:** `command_approval` (shell edit), `transform` pipeline, `defaultValue` expressions with `$cardId` ternaries, `fuzzyThreshold`. Those are pi-complete. Steal later if a real agent needs them.

`dryRun` is not optional. Without it, DAG tests need a TTY.

---

## 6. Result / history law

```json
{
  "success": true,
  "cancelled": false,
  "timed_out": false,
  "results": { "worker": "reader" },
  "answered": ["worker"],
  "skipped": ["confirm_deploy"],
  "count": 1
}
```

- `skipped` is **new** (condition/goto). Additive. Old agents ignore it.
- `secret` values: `results[id] = "***"` in protocol/history/dumps. Real value stays in a process-local map the **current** generate can use if we ever need it — by default the model does **not** see secrets echoed. If the model asked for a token, it should not also log it.
- Cancel: `success=false`, `cancelled=true`, empty results, **do not re-ask** (harness already says this; enforce with an ask-id fingerprint in the turn: same title+card ids within 1 generate → refuse).
- Timeout: `timed_out=true`, treat as cancel for control flow; message includes idle seconds.

`<result>` body is this JSON. Compact policy: `on_error: keep` already. Add `never_drop: [open_ask]` — a live/cancelled ask is not thought-noise.

---

## 7. Surgical slices (one commit each)

Each slice: propose → apply → `make test-ask-*` → install → live one card in TUI.

### S0 — inventory freeze (no behavior)

- Move `ask_protocol.hpp` → `src/tools/ask/protocol.hpp` (or keep path, add `src/tools/ask/` and include).
- Document the wire in `tool.yml` **as implemented**, not as wished. Demote unimplemented types to “engine v2” in the description so the model stops emitting `ranker` expecting magic.
- Delete or stub `src/tui/dialog.hpp` if it still compiles. One model.

Deletions: schema essays that teach DAG the engine cannot run.  
Keep: all current keys.

### S1 — secrets, timeout idle, Esc vs Ctrl-X, footer `ask`

- Redact `secret` in `askResult` + dump path.
- Idle timeout (keystroke resets). Defaults as §4.
- Test: Esc → `cancelled=true`, turn not `[CANCEL]`.
- Footer phase from `askActive`.
- `minDelaySecs` on `type_confirm` (TUI: ignore Enter until elapsed).

Tests that fail without this: secret appears in a fake dump string; confirm submits at t=0 with minDelay=3.

### S2 — AskEngine (headless DAG) + dryRun

- `AskEngine` owns cards, cursor, results, `condition`/`goto`/`branches`/`exitIf`.
- TUI `advanceDialog` becomes `engine.submit(value)`.
- Stdin fallback uses the same engine.
- `dryRun: true` returns `{cards, order}` without blocking.

Tests (no TUI):  
choice A → skip confirm (condition).  
choice B → goto confirm.  
exitIf on “no”.  
Unknown type → text (keep).  
dryRun does not call stdin.

This is the load-bearing slice. Do not paint until this is green.

### S3 — docked well + paint (visual)

Kill `drawAskDialog` modal. New `src/ui/chat/ask/paint.hpp`:
- Dock above prompt, below transcript (`askWellHeight` in the same bottom-up math as `promptBoxHeight`).
- Magenta ToolAsk rail. **No Square box. No cyan.**
- Per-type widgets §4.4. Composer is the text field; well is the question.
- Message scroll + option scroll. Selected always in view.
- Prompt restyle while ask live (placeholder = title / filter).
- Footer already `ask` from S1; here: `card 2/4` on occupancy.

Deletions: centered frame, `panel_2` fill, cyan border, inner `> █`.

Verify: `ui_view_test` fixtures in §4.8 all green. Live: 24-row term, 40-line message, last RESULT still visible above the well.

### S4 — AskSession queue + not-inside-feed

- Per-turn `AskSession` (id = action id). Queue if a second ask arrives.
- Dispatch ask **after** `generateStream` returns (batch of actions that includes ask_tool waits together; asks run after other sync tools in the same generation, or: asks always last in the batch).
- Never `requestAsk` from `parser.feed()`.

Test: scripted provider emits `list` + `ask_tool` in one generate; list result exists before the ask unblocks. Fail if ask CV is entered during feed.

### S5 — optionsResolver (small registry)

- Builtins: `imported_tools`, `imported_agents`, `sessions_open`, `providers`.
- Card `optionsResolver: "imported_agents"` filled at render time from Agent.
- No arbitrary tool-name callback in v1 (that's a footgun).

### S6 — diet

- `tool.yml` examples: **one** confirm, **one** choice. Kill the novels.
- `prompt_building.runtime_capabilities.usage_examples` already gates this — make ask_tool's schema short even when examples are on.

### Later (not this overhaul)

- `loop` / nested `chain`
- `command_approval`
- HTTP `needs_human` resume
- Persist unanswered ask across TUI restart (session file)
- `$cardId` defaultValue expressions

---

## 8. Tests (the ones that make this real)

| Test | Fails when |
|---|---|
| `test-ask-protocol` | result keys drift; secret not redacted |
| `test-ask-engine` | condition/goto/exitIf/dryRun wrong; linear tape regresses |
| `test-ask-timeout` | idle timer doesn't reset; wall timer used instead |
| `test-ask-dispatch` | ask runs inside feed; list+ask ordering wrong |
| `test-ask-cards` (existing) | stdin path broken |
| `ui_model_test` ask cases | parseDialogState / completeAsk regress |
| `ui_view_test` ask fixtures §4.8 | Square box / cyan / clipped 40-line message / secret leak / 16-row crush |

No live LLM required. ScriptedProvider + fake bridge.

---

## 9. What we will not do

- Clone pi's 20 card types in one PR.
- Keep two engines (stdin vs TUI).
- Block the SSE callback on a human.
- Put secrets in `iterations.md`.
- Teach the model a DAG in `tool.yml` before the engine exists.
- Steal the chat viewport for a yes/no.
- Treat Esc as Ctrl-X.
- Cyan Square modal (that is help overlay DNA).
- Second caret inside the well.
- Spinner / `sin()` / pulse on the idle clock.
- Green selected-style on a magenta ask list (wrong kind).

---

## 10. Suggested first move (when you say go)

**S0 + S2 skeleton in one sitting is tempting and wrong.**  
S0 (honest schema) is 20 minutes and stops the model from emitting fantasy types.  
S2 (engine) is the actual overhaul.  
S1 (secrets/timeout/footer) is the operator-visible honesty while S2 lands.

Order: **S0 → S1 → S2 → S4 → S3 → S5 → S6**.

S4 before S3 because a pretty well on a hung generate is still a hang.  
S3 before S5 because a resolver nobody can see is a box.

---

## 11. Files (expected, not a license to rewrite)

| Touch | Why |
|---|---|
| `src/tools/ask/engine.hpp` | new |
| `src/tools/ask/protocol.hpp` | move/keep wire |
| `src/ui/chat/ask_dialog_model.hpp` | thin wrapper over engine |
| `src/ui/chat/ask/paint.hpp` | well; replaces `drawAskDialog` |
| `src/ui/chat/ask/keys.hpp` | keymap per type |
| `src/ui/chat/chat_view.hpp` `drawAskDialog` | **delete** after paint.hpp is wired |
| `src/ui/scenes/agent_scene.hpp` | dock well in layout; stop overlay-after-surface |
| `src/ui/bridge/agent_bridge.hpp` | AskSession, idle timeout |
| `src/core/agent_tool_dispatch.cpp` | after-feed dispatch |
| `src/core/loop/stream.cpp` / `protocol.cpp` | do not call ask from feed |
| `manifests/built-in/tools/ask_tool/tool.yml` | honest schema + diet |
| `src/testing/ask_engine_test.cpp` | new |
| `src/tui/dialog.hpp` | delete or wrap if still linked |

Deletions (state explicitly when executing): duplicate oracle dialog if unused; schema example novels.

---

*Rate this. Then we execute S0 or you cut slices.*
