# Session Management Audit — Cortex MK3

**Date:** 2026-07-26 (authoritative rewrite — code-read, not free-model draft)  
**Branch:** `feat/inkcell-agentshell`  
**Repo:** `/home/mlamkadm/repos/active/agent-lib-cpp`  
**Live trigger:** operator semi-long run on `deepseek-v4-flash-free`  
**Scope:** persistence identity, dual-id / dual-writer, resume/fork, hub UX, CLI mental model, architecture target  
**Out of scope:** implementation, performance micro-opts (cross-link only), modularity extraction plan (cross-link only)

**Cross-links (peer audits, same day):**
- [2026-07-26-speed-performance-audit.md](./2026-07-26-speed-performance-audit.md) — blocking `persistUiTimeline`, pretty JSON, `commitAsync` joint
- [2026-07-26-code-quality-modularity-audit.md](./2026-07-26-code-quality-modularity-audit.md) — SessionController as P0.4 extract; dual TUI stacks

**Method:** primary evidence is file:line from this tree. Free-model narratives discarded. Tests skimmed as regression fences, not as proof of production correctness.

---

## 0. Executive verdict + grades

### Verdict

Session management **works on the happy path** and has been **vet-fix-hardened** against real operator failures (empty mint, empty-history wipe, Ctrl-C loss, resume thin projection). Those guards are real and must not regress.

Architecturally it is still a **dual-id, dual-store, dual-writer, multi-owner** system. There is **no single source of truth** for “what is the active session.” Identity policy lives in `main.cpp`, `ShellModel`, `Agent`, `flush::State`, and `dashboard_controller` simultaneously. Writers race without a mutex. Resume is **asymmetric** (hub prefers `ui_timeline`; CLI/`initializeChatModel` projects `records` only). Fork **drops** the structured timeline. State-checkpoint layout **does not match** what `list()` filters.

This is not a polish pass. It is an **identity + durability + operator-model** problem. North star: **one SessionController, one active id, one writer, one schema, hub ≡ CLI.**

### Grades

| Dimension | Grade | One-liner |
|-----------|-------|-----------|
| On-disk format (records + ui_timeline) | **B** | Usable JSON; atomic rename; no schema version |
| Create / lazy arm | **B−** | Phantom mint mostly fixed; dual arm paths (`chat-*` vs `sess-*`) |
| Resume live ↔ disk | **C+** | Hub uses `loadSessionUi`; CLI entry uses `loadSessionRecords` only |
| Fork | **D+** | Copies records/feeds/metadata; **omits** `uiTimelineJson` / `renderedHistory` |
| Exit durability | **B−** | atexit + multi-path flush; dual-id double write; no lock |
| Sessions hub UX | **C** | List/resume/create/export exist; weak identity; create ≠ CLI mint |
| Concurrency / write safety | **D** | Agent thread + UI tick + atexit; no Session write mutex |
| Operator mental model | **C−** | `--no-session` ⊥ `--ephemeral` correct in code; still overloaded in UX |
| Reusability (library surface) | **D** | Session policy embedded in UI model + Agent + main statics |
| Test fence | **B−** | lazy arm, timeline round-trip, live-resume parity, backfill, roundtrip |

**Bottom line:** stop adding vet-fix comments. Introduce **SessionController** with one active id, one write API, async commit (joint with perf), and hub actions that share CLI resolve/fork/metadata code.

---

## 1. What a session is (disk + process ids + dual store)

### 1.1 Canonical type

`Session` / `SessionRecord` — `src/core/types.hpp:165–185`:

```text
SessionRecord { Role: USER|AGENT|TOOL_CALL|TOOL_RESULT|SYSTEM; content; timestamp; metadata }
Session {
  id, agentName, model, provider, created, updated
  records[]
  metadata{}
  contextFeeds[]          // LLM-injected feeds; AC18 restore
  renderedHistory[]       // legacy ReplSession ANSI lines
  uiTimelineJson          // structured live transcript (experimental TUI)
}
```

Two UI projections of the same conversation:

| Projection | Consumer | Fidelity |
|------------|----------|----------|
| `records` | Agent `history_` ↔ LLM prompt | User/Agent/System strings; tool roles flattened on load |
| `uiTimelineJson` | ShellModel `rootRows` | Thoughts, actions, results, drill metadata — live parity |
| `renderedHistory` | legacy `src/tui/repl_session.hpp` | Pre-rendered lines; parallel stack |

### 1.2 Disk layout

| Artifact | Path (code) | Writer |
|----------|-------------|--------|
| Session JSON | `{cwd}/.cortex/sessions/<safe-id>.json` | `SessionManager::save` `manager.cpp:128–174` |
| State checkpoint | `{cwd}/.cortex/state/<safe-id>.json` | `Agent::saveStateCheckpoint` `agent_session.cpp:29–31, 486–506` |
| Dev dump (optional) | `$CORTEX_HOME/dev/<id>/` or `~/.cortex/dev/<id>/` | `dumpSessionArtifacts` `agent_session.cpp:56–75, 77–178` |

**Base dir default** (`manager.cpp:58–60`):

```text
baseDir_ = empty ? current_path() / ".cortex" / "sessions" : baseDir
```

Header comment claims `~/.cortex/sessions/` (`manager.hpp:4`) — **lie**. Reality is **CWD-relative**. Change directory → “lost” sessions. `CORTEX_HOME` is **not** used for session store (only for dev dumps / manifests). Tests that `setenv("CORTEX_HOME", …)` do **not** redirect session files unless they also chdir or pass baseDir.

**Atomic save** (`manager.cpp:170–174`): write `path.tmp` → `rename` → good.

**Pretty print always** (`manager.cpp:172`): `stringify(root, /*pretty=*/true)` — joint perf issue (see §9).

### 1.3 Dual store (records file vs checkpoint)

| Concern | Session file | State checkpoint |
|---------|--------------|------------------|
| Purpose | Operator-visible transcript + metadata + ui_timeline | Full agent runtime (history, pins, peeks, executed actions, sub-agents) |
| Path | `.cortex/sessions/<id>.json` | `.cortex/state/<id>.json` |
| Gate | content / id rules in `saveSession` | **requires** session file already exists (`agent_session.cpp:492–494`) |
| Recovery | primary | used when `records:[]` but history recoverable (`loadSession` 229–265) |

**Layout mismatch (F4):**

- Checkpoint writes to **`.cortex/state/<id>.json`** (`agent_session.cpp:29–31`).
- `SessionManager::list` filters sibling **`<id>.state.json` under sessions dir** (`manager.cpp:211–217`).

Comments on both sides talk about “sibling `.state.json`.” The filter is defensive against a **different** layout than the one `saveStateCheckpoint` actually uses. Listing is safe; the **mental model and docs are wrong**. Checkpoint is a second tree, not a sibling.

### 1.4 Process identity — three “current session” slots

| Slot | Owner | Set by |
|------|-------|--------|
| `CliConfig.sessionId` / `activeSessionId` local in `main` | CLI resolve / fork / continue | `main.cpp:1949–2001`, `resolveSessionId` 1057–1088 |
| `InkcellAppConfig.sessionId` | App entry config | assigned before `runInkcell*` (`main.cpp:2266, 2326, …`) |
| `ShellModel.activeSessionId` | UI model | `initializeChatModel` copies cfg (`mk3_tui_app.hpp:41`); **lazy arm** may mint new (`inkcell_app_model.hpp:1431–1446`); hub resume/create overwrite (`main_scene.hpp:1659, 1676`) |
| `flush::State.{sessionId, cfgSessionId, agent*}` | atexit safety net | `activate` / `setActiveSession` (`mk3_tui_app.hpp:130–138`) |
| `Agent::lastSessionId_` | in-process load policy | `agent.cpp:348–360` |

Exit paths flush **cfg id and model id** (often same string; not guaranteed after lazy arm or hub create):

```text
mk3_tui_app.hpp:276–277  runInkcellShell
mk3_tui_app.hpp:344–345  runInkcellOneShot
mk3_tui_app.hpp:158–159  flush::runOnce (atexit) — cfgSid + sid
```

That dual-id flush is the **smoking gun**.

### 1.5 Writers (dual / multi)

| Writer | Call site | What it writes |
|--------|-----------|----------------|
| `Agent::saveSession` | `agent_session.cpp:268–335`; end of `prompt` `agent.cpp:2130–2141`; `submitComposer` arm `inkcell_app_model.hpp:1457`; `flushAgentSession` 356–358 | records + contextFeeds + identity merge |
| `ShellModel::persistUiTimeline` | TurnDone `inkcell_app_model.hpp:1228`; route/quit ticks; REPL exit 581 | load-merge → `uiTimelineJson` only |
| `persistSessionMetadata` | `main.cpp:1116–1145` at TUI launch | create-or-load + metadata paths + agent identity |
| `Agent::saveStateCheckpoint` | after `saveSession` in prompt | full runtime JSON under `.cortex/state/` |
| atexit `flush::runOnce` | process exit | dual `flushAgentSession` |

**No global write mutex.** UI thread can `persistUiTimeline` while worker `saveSession` and atexit may run. Last writer wins; partial interleaving possible on large sessions.

---

## 2. Lifecycle with evidence

### 2.1 Bare launch (no `-c/-r/--session/--fork`)

```text
resolveSessionId(cli, false)  → "" when no flags and no id   main.cpp:1086–1088
experimentalSessionId empty   → no persistSessionMetadata    main.cpp:2311–2315
cfg.sessionId = ""
model->activeSessionId = ""   initializeChatModel            mk3_tui_app.hpp:41
```

**Good:** no phantom zero-record file on open. Comment at `main.cpp:2248–2251` matches intent.

### 2.2 First typed prompt (lazy arm)

`ShellModel::submitComposer` (`inkcell_app_model.hpp:1414–1470`):

1. If `activeSessionId.empty()` → mint `sess-%016llx-%04llx` (steady_clock + atomic counter).
2. `seedUserPrompt(text)` on agent (`agent.hpp:126–128`).
3. Immediate `saveSession(activeSessionId)` so Ctrl-C before first iteration still has User record.
4. Push User row; set `pendingSubmit`.

Worker later: `runAgentTurn(..., model->activeSessionId, noSession, ...)` (`mk3_tui_app.hpp:549`).

`Agent::prompt` load policy (`agent.cpp:338–360`): if history already seeded, **do not** reload from disk (avoids wiping Agent accumulation).

End of turn: `saveSession` + `saveStateCheckpoint` if content (`agent.cpp:2130–2141`).  
TurnDone on UI: `persistUiTimeline()` (`inkcell_app_model.hpp:1226–1229`).

### 2.3 Explicit session / continue / resume

| Flag | Behavior | Evidence |
|------|----------|----------|
| `--session <id>` | use id | `resolveSessionId` 1086–1087 |
| `-c / --continue` | most recent session with AGENT/TOOL_* record; else newest | 1065–1084 |
| `-r / --resume` | interactive picker; cancel → empty → clean exit | 1058–1063, 1979–1982 |
| `--name` | metadata `name`; list prefers it over agentName | `persistSessionMetadata` 1138–1141; `list` 224–231 |
| `--quiet-session` | suppress banner | 580, 2004 |
| `--show-history N` | CLI field exists | `CliConfig` 109 — paint path is TUI-dependent |

Resume banner: `printResumeBanner` `main.cpp:1182–1220`.

Metadata restore: `applySessionMetadata` 1091–1114 (manifest/harness/system/persona/provider/model from session.metadata if CLI unset).

### 2.4 Fork

`main.cpp:1954–1974`:

```text
load src → create newId → copy records, contextFeeds, metadata
set metadata["forked_from"]
save fork → cli.sessionId = newId
```

**Not copied:** `uiTimelineJson`, `renderedHistory`.  
Forked resume of experimental TUI therefore falls back to **thin records projection** — thoughts/actions/drill gone.

### 2.5 Hub create / resume

| Action | Code | Behavior |
|--------|------|----------|
| Create | `createDashboardSession` `dashboard_controller.hpp:41–67` | mint `chat-<ms>`; **no** `sessions.create()`; clear agent history; arm id only |
| Resume | `resumeDashboardSession` + `main_scene.hpp:1611–1661` | load agent session; **prefer** full session + `loadSessionUi` (ui_timeline) |
| Export | `main_scene` ~1727+ | portable via `exportToFile` |

Hub create id prefix **`chat-`** vs lazy arm **`sess-`** vs CLI `newSessionId()` — three mint shapes (F6).

### 2.6 Exit / signal

| Path | Order | Evidence |
|------|-------|----------|
| REPL normal exit | `persistUiTimeline` then `flushAgentSession(slot)` | `mk3_tui_app.hpp:576–584` — timeline first (comment intentional) |
| Shell exit | dual flush cfg + model | 276–279 |
| One-shot exit | dual flush; respects `noSession` on cfg id only | 344–345 — model id flushed with `false` |
| atexit | if still `active`, flush both ids | 143–159; `installAtexit` 163–167 |
| Route to main / quit tick | `persistUiTimeline` | 77–81, 529–533 |

`flushAgentSession` (`356–358`): `if (ephemeral \|\| id.empty()) return; agent.saveSession(id);` — **does not** call `persistUiTimeline` or checkpoint.

### 2.7 `--no-session` vs `--ephemeral`

Documented orthogonal (`main.cpp:91–95, 234, 280`):

| Flag | Meaning in code |
|------|-----------------|
| `--no-session` | empty session id into prompt / no metadata persist; `prompt(..., ephemeral=true)` when used as third arg in some paths |
| `--ephemeral` | exit-on-done for experimental TUI (`icfg.ephemeral`); **not** the same as no persist |

**Trap:** `runAgentTurn` third bool is named `ephemeral` but REPL passes **`noSession`** (`mk3_tui_app.hpp:549`). So `Agent::prompt`'s `ephemeral` flag is driven by **no-session**, not by exit-on-done.  
`--ephemeral` alone still saves if an id is armed. Correct for “exit when done but keep disk,” easy to misread.

Legacy ReplSession collapses: `replCfg.ephemeral = cli.noSession` (`main.cpp:2355`) — **dual stack semantic drift**.

### 2.8 Hot-swap (hub launch)

`runInkcellRepl` hub launch (`mk3_tui_app.hpp:496–520`): build new Agent from manifest, `applyLiveIdentity` **clears transcript**, re-`flush::activate(slot->get(), model->activeSessionId)`.

**Risk:** same `activeSessionId` now bound to a **different** agent; `saveSession` identity-merge rules try not to clobber non-empty on-disk agentName (`agent_session.cpp:284–292`), but history_ is the new agent’s. Operator can resume a session whose disk identity and live agent diverged mid-run.

### 2.9 saveSession / loadSession policy (Agent)

**loadSession** (`agent_session.cpp:184–266`):

- clear history/feeds/actionResults
- load JSON; backfill empty agentName/model/provider from config and re-save
- project records → `User:/Agent:/System:` history lines (**TOOL_* collapse to System**)
- restore contextFeeds
- if history empty → load checkpoint → rebuild records → save session (repair path)

**saveSession** (`268–335`):

- empty id → return
- exists: load, fill empty identity only, **refuse to wipe non-empty records with empty history_** (metadata touch only)
- !exists and empty history+feeds → return (no orphan)
- else rewrite records from history_; set contextFeeds; save

These guards exist because dual-id exit flush used the **wrong Agent** after hot-swap (comment 294–298). Architecture debt made concrete.

---

## 3. What works (do not regress) + tests

### 3.1 Working invariants (keep)

1. **No phantom file on bare open** — resolve with `defaultIfEmpty=false`; create gated on content.  
2. **Lazy arm on first real submit** — operator work always gets an id.  
3. **seedUserPrompt + immediate save** — mid-submit abort still has User record.  
4. **prompt() does not reload over seeded history** — User+Agent round-trip.  
5. **Empty history cannot wipe non-empty disk records** — hot-swap / wrong-agent flush defense.  
6. **Checkpoint gated on session exists** — no orphan checkpoint-only rows.  
7. **ui_timeline filters Stream/Status** — `timelineRowPersistable` `inkcell_app_model.hpp:212–218`.  
8. **Hub resume prefers ui_timeline** — `loadSessionUi` + `main_scene.hpp:1631–1657`.  
9. **tmp+rename save** — crash mid-write less likely to corrupt.  
10. **atexit safety net** — SIGINT paths that skip normal return still flush if armed.  
11. **`--no-session` ⊥ `--ephemeral`** at CLI flag level (even if naming in `runAgentTurn` confuses).  
12. **list() sorts by updated desc; metadata name preferred** for display.

### 3.2 Test fence (skim)

| Test | Path | Asserts |
|------|------|---------|
| Lazy arm | `src/testing/lazy_session_test.cpp` | empty → submit → non-empty id; User record on disk |
| UI timeline codec | `src/testing/ui_timeline_test.cpp` | serialize drops stream/status; round-trip kinds/drill |
| Live resume parity | `src/testing/live_resume_parity_test.cpp` | persistUiTimeline → loadSessionUi row equality |
| Session roundtrip | `src/testing/session_roundtrip_test.cpp` | seed + prompt leaves User **and** Agent |
| Load backfill | `src/testing/load_backfill_test.cpp` | empty identity + records:[] + checkpoint repair |
| Session unit | `src/testing/session_test.cpp` | AC14 provider, AC04 created stamp, etc. |

**Gaps:** no dual-id flush race test; no fork-copies-ui_timeline test; no initializeChatModel vs hub resume asymmetry test; no write-mutex / concurrent save test; no CWD vs CORTEX_HOME session store test; no atexit dual-flush integration test.

---

## 4. Failure catalog F# + file:line

| ID | Severity | Failure | Evidence |
|----|----------|---------|----------|
| **F1** | **P0** | **Dual session id** — cfg vs model can diverge; exit flushes both | `mk3_tui_app.hpp:276–277, 344–345, 158–159`; lazy arm `inkcell_app_model.hpp:1431–1446` vs `cfg.sessionId` |
| **F2** | **P0** | **Dual / multi writer** — Agent saveSession, persistUiTimeline load-merge-save, main metadata, atexit; no mutex | `agent_session.cpp:268`; `inkcell_app_model.hpp:1342–1366`; `main.cpp:1116–1145`; `mk3_tui_app.hpp:143–159` |
| **F3** | **P0** | **Resume asymmetry** — CLI/`initializeChatModel` uses `loadSessionRecords` only; hub uses `loadSessionUi` | `mk3_tui_app.hpp:56–58` vs `main_scene.hpp:1631–1657` / `inkcell_app_model.hpp:1321–1337` |
| **F4** | **P1** | **Checkpoint path ≠ list filter story** — state under `.cortex/state/<id>.json`; list filters `*.state.json` under sessions | `agent_session.cpp:29–31`; `manager.cpp:211–217` |
| **F5** | **P0** | **Fork drops ui_timeline (and renderedHistory)** | `main.cpp:1960–1970` — only records, contextFeeds, metadata |
| **F6** | **P1** | **Three id mint schemes** — `sess-*` lazy, `chat-*` hub, CLI `newSessionId()` | `inkcell_app_model.hpp:1442–1445`; `dashboard_controller.hpp:47–50`; `main.cpp:855+` |
| **F7** | **P1** | **CWD-relative session store; header docs wrong; CORTEX_HOME ignored** | `manager.cpp:58–60`; `manager.hpp:4` |
| **F8** | **P1** | **TOOL_CALL / TOOL_RESULT → System on load** — role fidelity lost for LLM history | `agent_session.cpp:206–220` (default → System) |
| **F9** | **P1** | **persistUiTimeline can create session shell without records** if file missing | `inkcell_app_model.hpp:1351–1357` then save with ui_timeline only |
| **F10** | **P0** (perf joint) | **UI-thread load-merge-pretty-save on TurnDone** | `inkcell_app_model.hpp:1228, 1342–1364`; `manager.cpp:172` |
| **F11** | **P1** | **atexit agent pointer may be dead/wrong after hot-swap if activate missed** | `flush::activate` on launch 519; shell path activates CLI agent 263–264 |
| **F12** | **P2** | **One-shot dual flush: model id ignores noSession** | `mk3_tui_app.hpp:344–345` — second call `false` not `noSession` |
| **F13** | **P1** | **Hot-swap clears transcript but keeps activeSessionId** — identity/history mismatch risk | `applyLiveIdentity` clearTranscript 506; same session id 519 |
| **F14** | **P2** | **export portable omits ui_timeline / context_feeds / metadata richness** | `manager.cpp:258–284` — messages only |
| **F15** | **P2** | **prune keeps first record only heuristic; rarely called from product paths** | `manager.cpp:246–255` |
| **F16** | **P1** | **Naming overload: runAgentTurn `ephemeral` param is noSession from REPL** | `mk3_tui_app.hpp:170, 549` |
| **F17** | **P2** | **Legacy ReplSession: ephemeral == noSession** — dual stack | `main.cpp:2355` |
| **F18** | **P2** | **No schema `version` on main session file** (checkpoint has version=1) | session JSON vs `stateCheckpointJson` `agent_session.cpp:340` |
| **F19** | **P1** | **flush does not persist ui_timeline** — SIGINT before TurnDone can lose structured rows (records may still save via seed/prompt) | `flushAgentSession` 356–358 vs TurnDone 1228 |
| **F20** | **P2** | **catch (...) swallows load/save/timeline errors** | `manager.cpp:124–125`; `persistUiTimeline` 1365–1366; checkpoint 480–482 |

---

## 5. UX / UF hub + CLI mental model

### 5.1 Operator-facing CLI surface

```text
-c / --continue      most recent “real” session
-r / --resume        picker
--session <id>       pin id
--name <name>        display label in metadata
--fork <id>          copy records → new id (timeline lost — F5)
--no-session         do not load/save
--ephemeral          exit when turn done (persist still possible)
--quiet-session      no banner
sessions list|show|rm|export
```

### 5.2 Hub surface (Sessions section)

- List via `DashboardState::refreshSessions` → `SessionManager::list`
- Active highlight: `isSessionActive` / main_scene comparisons to `activeSessionId`
- Resume: full `loadSessionUi` path (better than CLI entry)
- Create: arm-only `chat-*` id, clear history, no file until content
- Export: portable JSON (thin)

### 5.3 Mental model fractures

1. **“Where are my sessions?”** — CWD `.cortex/sessions`, not `~/.cortex`, not `$CORTEX_HOME`.  
2. **“Did bare TUI create a session?”** — No until first submit (good) or until hub Create arm + first save.  
3. **“Does --ephemeral mean no disk?”** — No. That is `--no-session`.  
4. **“Resume looks different from live”** — if you resumed via CLI path without hub, F3.  
5. **“Fork keeps the chat look”** — No; F5.  
6. **“Two files for one chat?”** — sessions JSON + state JSON; plus optional ui_timeline inside sessions.  
7. **Id soup** — `sess-…`, `chat-…`, CLI-generated ids, optional `--name`.

### 5.4 Desired operator model (target)

```text
Session = durable conversation identity
  · one id always
  · open = resume or new
  · fork = full deep copy (records + ui_timeline + feeds + metadata)
  · --no-session = RAM only
  · --ephemeral = process exits after turn (orthogonal)
  · store root = $CORTEX_HOME/sessions or XDG, not silent CWD
  · hub and CLI share one resolve/create/fork API
```

---

## 6. Architecture target — SessionController

### 6.1 Why

Today: **five owners**, **three mints**, **four writers**, **two resume paths**, **two TUI stacks**. Vet-fix comments are a distributed design doc. Reusability north star (modularity audit): headless automation must load → run → save **without** `src/ui/**`.

### 6.2 Proposed surface (design only)

```text
session::SessionController
  · activeId() / setActiveId() / armIfEmpty(policy)   // single id
  · resolve(CliIntent)                                 // -c/-r/--session/--fork
  · open(id) / create(meta) / fork(fromId) → full copy
  · commit(Snapshot) / commitAsync(Snapshot, gen)      // single writer + mutex
  · loadForAgent(Agent&) / loadForUi(ShellModel&)      // one resume policy
  · flush()                                            // atexit + normal exit
  · list() / remove() / export()                       // thin over SessionManager

Snapshot {
  id, generation,
  records? | history_lines?,
  uiTimelineJson?,
  contextFeeds?,
  metadata?, identity{agent,model,provider},
  flags{ noSession, ephemeralExit }
}
```

### 6.3 Ownership rules

| Rule | Detail |
|------|--------|
| One active id | ShellModel and cfg read controller; never mint locally |
| One writer | only controller commits; Agent/UI produce Snapshot |
| One resume | always prefer ui_timeline then records then checkpoint repair |
| Fork | deep copy all Session fields |
| Store root | explicit; env-overridable; documented |
| Checkpoint | same controller; path scheme documented; list filter matches |
| atexit | controller.flush once; no dual-id loop |

### 6.4 Placement

`src/session/controller.{hpp,cpp}` (or extend manager carefully).  
Callers to delete dual logic from: `mk3_tui_app.hpp` dual flush, `submitComposer` mint, `main.cpp` fork/metadata (move, don’t duplicate), `persistUiTimeline` direct `sm.save`.

Aligns modularity audit **P0.4** and perf audit **Perf0.1**.

---

## 7. P0 / P1 / P2 + acceptance

### P0 — correctness / identity (block features that touch sessions)

| ID | Work | Acceptance |
|----|------|------------|
| **S0.1** | Single active session id API; delete dual flush | One id in process; exit/atexit call flush once; lazy_session + existing tests green |
| **S0.2** | Single writer + mutex (sync first) | Concurrent TurnDone + saveSession + exit cannot interleave JSON; generation coalesce optional |
| **S0.3** | Unified resume: `loadSessionUi` everywhere (CLI entry + initializeChatModel) | Live-resume parity test extended to initializeChatModel path; no records-only thin paint when ui_timeline exists |
| **S0.4** | Fork deep-copies `uiTimelineJson` (+ renderedHistory if kept) | New test: fork → loadSessionUi row parity with source |
| **S0.5** | `commitAsync` skeleton joint with perf | UI thread ≤ 8 ms after TurnDone (perf gate); file eventually consistent |

### P1 — durability / model clarity

| ID | Work | Acceptance |
|----|------|------------|
| **S1.1** | Fix store root (CORTEX_HOME/XDG) + honest docs | Same machine, different CWD, same sessions when env set |
| **S1.2** | Align checkpoint path with docs/list (or stop claiming sibling) | One documented layout; list never depends on wrong suffix |
| **S1.3** | One mint policy / one id format | Hub create, lazy arm, CLI share `SessionController::mint()` |
| **S1.4** | Flush also commits ui_timeline snapshot (or guarantees TurnDone already did) | SIGINT after partial turn: structured rows not silently dropped when rootRows non-empty |
| **S1.5** | Preserve TOOL_* roles through history projection or document loss | load/save role round-trip test |
| **S1.6** | Rename/clarify noSession vs ephemeral params in runAgentTurn | Code + help text unambiguous; legacy mapping isolated |

### P2 — hygiene / scale

| ID | Work | Acceptance |
|----|------|------------|
| **S2.1** | Session JSON schema version + size policy (compact default) | version field; pretty opt-in |
| **S2.2** | Portable export includes ui_timeline / feeds / metadata | import restores experimental paint |
| **S2.3** | Hot-swap session policy (new id vs bind vs confirm) | Explicit operator choice; no silent identity drift |
| **S2.4** | Fail loud on save/load errors (not catch-all) | Operator-visible notice |
| **S2.5** | Race / atexit / dual-id regression tests | CI gates |

### Explicit non-goals (this audit)

- Implementing SessionController in this document  
- Killing `src/tui` (modularity P2.4)  
- Token/drain performance (perf audit owns numbers)

---

## 8. Live-run correlation

**Trigger:** semi-long operator run on `deepseek-v4-flash-free` (slow free model → long wall time, many tokens, multi-iteration tools).

| Observed class (typical) | Code mechanism |
|--------------------------|----------------|
| End-of-turn freeze before composer free | **F10** — `persistUiTimeline` on UI thread + pretty full-session rewrite |
| Quit / Ctrl-C anxiety (“did it save?”) | **F1/F19** — dual flush + atexit saves records but not necessarily ui_timeline |
| Resume thinner than live | **F3** if entry via CLI initialize path; hub path OK |
| First Enter latency spike | lazy arm + sync `saveSession` `inkcell_app_model.hpp:1457–1458` (perf P2) |
| Growing sluggishness over long chat | rebuild culture + larger pretty JSON (perf audit); session size amplifies F10 |
| “Empty session” / wrong agent after hub launch | **F13** + historical wipe guards in saveSession |

Free-model slowness **amplifies** session bugs: more time for SIGINT, larger timelines, more TurnDone persists, more chance dual writers overlap. Session correctness is not optional under slow providers.

---

## 9. Joint with perf (`commitAsync`) + modularity (extract-as-we-go)

### 9.1 Perf audit joint

From [speed-performance-audit](./2026-07-26-speed-performance-audit.md):

| Perf item | Session item | Shared fix |
|-----------|--------------|------------|
| Perf0.1 async session commit | S0.5 / S0.2 | `SessionController.commitAsync` |
| Pretty `stringify` every save | S2.1 | compact default; pretty debug-only |
| Dual flush exit latency | F1 / S0.1 | single flush |
| First-submit sync save | lazy arm path | enqueue commit, not block Enter |
| Long-session soak | ui_timeline growth | size policy + async |

**Do not** “fix” F10 by deleting ui_timeline. Fix by **async single-writer commit**.

### 9.2 Modularity audit joint

From [code-quality-modularity-audit](./2026-07-26-code-quality-modularity-audit.md):

| Modularity | Session |
|------------|---------|
| P0.4 SessionController | **this audit’s architecture target** |
| P0.5 CLI peel | `resolve/fork/metadata` leave `main.cpp` |
| UI must not own disk policy | delete direct `sessionMgr().save` from ShellModel |
| Headless reuse | Agent produces Snapshot; controller persists |

**Extract-as-we-go rule:** any new session behavior (hub actions, fork flags, resume paint) lands **behind SessionController**, not another vet-fix in `inkcell_app_model.hpp`.

### 9.3 Suggested sequence (no implementation here)

```text
1. S0.1 single id + delete dual flush          (small, high leverage)
2. S0.3 unified loadSessionUi on all entries   (correctness visible)
3. S0.4 fork deep copy                         (one function)
4. S0.2 write mutex around SessionManager::save
5. S0.5 commitAsync + compact JSON             (joint perf)
6. S1.* store root, mint unify, role fidelity
7. Extract controller module; peel main.cpp statics
```

---

## References

### Primary sources (read for this rewrite)

| File | Role |
|------|------|
| `src/session/manager.hpp` | SessionManager API |
| `src/session/manager.cpp` | load/save/list/create/prune/export; pretty write; state.json list filter |
| `src/core/types.hpp` | Session, SessionRecord |
| `src/core/agent.hpp` | session API, seedUserPrompt, sessionMgr() |
| `src/core/agent_session.cpp` | load/save/checkpoint/clear/undo/dump |
| `src/core/agent.cpp` | prompt load policy; end-of-turn save |
| `src/ui/model/inkcell_app_model.hpp` | activeSessionId, serialize/load/persist timeline, submitComposer lazy arm |
| `src/ui/app/mk3_tui_app.hpp` | initializeChatModel, dual flush, atexit, runInkcell* |
| `src/ui/model/dashboard_controller.hpp` | resume/create arm-only |
| `src/ui/model/dashboard_model.hpp` | list/selection/active flag |
| `src/ui/scenes/main_scene.hpp` | hub resume loadSessionUi; create; export |
| `main.cpp` | resolve, fork, metadata, flags, TUI launch |
| `src/testing/lazy_session_test.cpp` | lazy arm fence |
| `src/testing/live_resume_parity_test.cpp` | ui_timeline parity |
| `src/testing/ui_timeline_test.cpp` | codec |
| `src/testing/session_roundtrip_test.cpp` | User+Agent |
| `src/testing/load_backfill_test.cpp` | repair path |

### Peer audits

- `AUDITS/REPORTS/2026-07-26-speed-performance-audit.md`
- `AUDITS/REPORTS/2026-07-26-code-quality-modularity-audit.md`

---

*End of authoritative session management audit. Dual-id/dual-writer is the structural bug; SessionController + commitAsync is the joint fix with perf and modularity. No implementation in this document. GODSPEED.*
