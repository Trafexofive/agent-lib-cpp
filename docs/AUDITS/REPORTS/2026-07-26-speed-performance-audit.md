# Speed & Performance Audit — Cortex MK3 (+ inkcell boundary)

**Date:** 2026-07-26  
**Branch:** `feat/inkcell-agentshell`  
**Repo:** `/home/mlamkadm/repos/active/agent-lib-cpp`  
**Trigger:** Operator semi-long live run on `deepseek-v4-flash-free` + full hot-path re-read  
**Method:** Code-authoritative (file:line). Free-model discovery used only as index. No implementation in this pass.  
**Cross-links:**
- [2026-07-26-session-management-audit.md](./2026-07-26-session-management-audit.md) — dual-writer, pretty JSON, joint `commitAsync`
- [2026-07-26-code-quality-modularity-audit.md](./2026-07-26-code-quality-modularity-audit.md) — `TimelineCodec` / `RowPolicy` / `EventReducer` extractions that unlock perf work

**North star:** keep parser/reducer wins; kill **amortized per-frame work**, **UI-thread disk**, and **rebuild culture** under long sessions.

---

## 0. Executive verdict + layer grades

| Layer | Grade | One-liner |
|-------|:-----:|-----------|
| **Provider TTFT / free-tier** | n/a | External variance; Cortex does not own this |
| **Parser feed** | **A** | Streaming-linear; budgets pass with large margin |
| **Protocol → UI diff** | **A−** | Dirty-index only; RETRY contract correct |
| **Bridge coalesce** | **A−** | 16 ms publish + `publishMany` + eventfd; snapshot bounds stream events |
| **Engine wake / tick** | **B+** | 33 ms poll + wake_fd; on_wake deliberately empty (correct) |
| **Model drain / reducer** | **B** | One rebuild per batch gated; rebuild itself still O(rows×body lines) |
| **Row policy / caps** | **C+** | 8–16 KiB body + 600-row cap exist; cap eviction is O(n) erase |
| **Transcript wrap cache** | **B+** | Version×width + dirty-tail rewrap; invalidated by full rebuild culture |
| **Draw / Surface / diff** | **B** | inkcell diff_frame + surface reuse; Cortex still feeds large string vectors |
| **Session persist (side path)** | **C−** | TurnDone → serialize + load-merge + **pretty** save on UI thread |
| **Cancel path** | **C+** | `g_running` works; Engine hard-quits on Ctrl-C before scene cancel |
| **Long-session jank** | **C** | Cap hitch + rebuild + persist dominate after many turns |
| **Legacy ReplSession** | **C** | Separate poll profile (2/50 ms); dual stack cost |
| **Overall** | **B−** | Streaming core is production-grade; long-run UX is not |

**Verdict:** The historic disasters (parser 40–400×, reducer ~8×, unbounded bridge history, full transcript rewrap every frame) are **fixed and gated**. What a semi-long free-model run still surfaces is not “parser too slow” — it is:

1. **Rebuild culture** — every protocol batch rewrites all display lines  
2. **Blocking persist** — `persistUiTimeline` + pretty `SessionManager::save` on the UI tick  
3. **O(n) cap erase** — `vector::erase` from front when `kRootRowCap` bites  
4. **UX latency affordances** — cancel/Esc/scroll compete with drain+draw on a god model  
5. **Missing gates** — no long-session, no cap-hitch, no non-blocking persist probe  

**Bottom line:** do **not** re-optimize the parser. Optimize **amortized per-frame work**, **never block the UI thread on disk/JSON**, and extract pure modules (`TimelineCodec` / `RowPolicy` / `EventReducer`) so the next UI consumer inherits the same budgets.

---

## 1. Hot path map (token → terminal + persist side path)

### 1.1 Live streaming path (experimental inkcell shell)

```text
LLM provider callback
  → Agent::runLoop onToken (parser.feed per token)          agent.cpp ~1834–1864
  → runAgentTurn onToken coalesce                           mk3_tui_app.hpp:201–248
       · protocol dirty? → flush immediately
       · pure bytes?     → ≤1 publish / 16 ms + final flush
       · collectProtocolChanges (index-diff only)
  → AgentBridge::publishMany + eventfd write                agent_bridge.hpp:67–84, 185–191
  → inkcell Engine select(stdin, wake_fd) @ input_poll 33ms engine.hpp:276–305
  → on_tick → ShellModel::drain(bridge)                     mk3_tui_app.hpp:62–85
       · batchingEvents=true; apply each UiEvent
       · one rebuildViews if pending
       · enforceRowCap after batch
  → rebuildViews → transcriptView.lines + transcriptVersion inkcell_app_model.hpp:708–886
  → AgentScene::draw → ChatSurfaceModel + drawChatSurface   agent_scene.hpp:297–376
  → drawTranscript: wrap cache (version×width, dirty tail)  chat_view.hpp:438–481
  → inkcell Surface paint + Renderer::diff_frame            engine.hpp:266–274
  → terminal write
```

**Coalesce design (correct, keep):**

| Stage | Interval | Evidence |
|-------|----------|----------|
| Worker publish | 16 ms when only bytes dirty | `mk3_tui_app.hpp:201–229` |
| UI drain/rebuild | once per Engine tick (~33 ms) | `installAppTick` 62–72; comment 63–69 explicitly forbids on_wake drain |
| Engine wake | eventfd readable → select returns early | `engine.hpp:90–100, 285–303` |
| Nested drill refresh | ≥100 ms wall | `refreshNested` 979–997 |

### 1.2 Persist side path (the long-run hang)

```text
UiEventKind::TurnDone
  → ShellModel::apply TurnDone arm
  → persistUiTimeline()                 inkcell_app_model.hpp:1228, 1342–1366
       · serializeTimeline(rootRows)    compact JSON writer (good)
       · sessionMgr().load(id)          full session parse
       · s.uiTimelineJson = json
       · sessionMgr().save(s)           pretty-print WHOLE session  manager.cpp:128–175
  → rebuildViews again
  (+ exit/atexit) flushAgentSession + dual-id flush          mk3_tui_app.hpp:158–159, 276–277
  (+ first submit) saveSession on arm                        inkcell_app_model.hpp:1457–1458
```

**Critical coupling:** TurnDone is delivered on the **UI thread** via `drain` inside `on_tick`. JSON serialize + disk I/O run **before** the next draw returns. Large `rootRows` / large `records` → “model finished, TUI freezes.”

### 1.3 Cancel path

```text
Ctrl-X / palette stop → AgentScene::stopAgentLoop → g_running=false   agent_scene.hpp:624–636
SIGINT (process)      → main signalHandler → g_running=false          main.cpp:195–199
                      + inkcell g_signal_received → engine exit       engine.hpp:208–222
Ctrl-C key (TTY)      → Engine HARD QUIT before scenes_.on_key        engine.hpp:314–318
                        AgentScene Ctrl-C handler is unreachable for
                        Engine-owned input (dead code for stop intent)
Agent loop polls g_running between iterations / backoff               agent.cpp:845, 1815–1824
```

Cancel **can** stop the agent (global flag). Operator-perceived cancel is still gated by: next drain frame, provider stream exit, and any in-flight `persistUiTimeline` / `saveSession`.

### 1.4 Legacy ReplSession path (dual stack)

```text
waitForActivity(streaming ? 2ms : 50ms) poll(stdin, wakeFd)   repl_session.hpp:735–748
```

Different cadence, different history model (`renderedHistory`), separate perf profile. Product cost: two places to fix jank.

---

## 2. Measured gates (current) + gate gaps

### 2.1 Documented + coded contract

Source of truth: `docs/CORTEX_PERFORMANCE.md` + `src/testing/perf_test.cpp`.

| Gate | Budget | Live measure (this machine, `make test-perf`) | Intent |
|------|-------:|-----------------------------------------------:|--------|
| 256 KiB response / 1-byte chunks | 250 ms | **7 ms** PASS | Parser streaming response |
| 128 KiB action / 1-byte chunks | 300 ms | **48 ms** PASS | Nested/action scanner |
| 1000 protocol updates / one drain | 100 ms | **2 ms** PASS | Reducer batch + **single rebuild** |
| 3000-line cached transcript / 200 frames | 150 ms | **43 ms** PASS | Wrap cache reuse |

Also asserted in `reducer_batch_budget()` (`perf_test.cpp:88–116`):

- indexed response integrity (`rootRows` size 1, body length == last response)  
- `viewRebuildCount == before + 1`  
- bridge snapshot excludes Token/Protocol retention (`events.empty()` after pure stream batch)

Historical before/after (from `CORTEX_PERFORMANCE.md`, 2026-07-12, `-O2`):

| Workload | Before | After |
|----------|-------:|------:|
| 256 KiB response 1-byte | 322 ms | 7–9 ms |
| 64 KiB action 1-byte | 1272 ms | 8 ms |
| 128 KiB action 1-byte | 5073 ms | 12–13 ms |
| 4000 response mutations / drain | 319 ms | 40 ms |
| 3000-line cached / 200 frames | n/a | 9 ms |

### 2.2 Gate gaps (what long runs actually need)

| Missing gate | Why it matters | Suggested budget (starting point) |
|--------------|----------------|-----------------------------------|
| **Cap enforcement hitch** | `enforceRowCap` + `erase(begin,…)` under 600+ rows | drop 100 Stream/Thought from 700-row store ≤ 2 ms |
| **Non-blocking persist** | TurnDone must not stall draw | UI thread work after TurnDone ≤ 8 ms; disk off-thread |
| **Serialize 2k-row timeline** | Compact JSON cost grows with session | serialize ≤ 15 ms @ 2k rows; async commit |
| **Pretty save of large session** | `stringify(..., pretty=true)` | compact path ≤ ½ pretty time |
| **rebuildViews @ 600 rows** | full label/body re-emit | ≤ 8 ms steady-state |
| **Dirty-tail wrap only** | already coded; not stress-tested with growing body | streaming 200 appends of last line ≤ 50 ms total |
| **Nested refresh rate** | `rowsFromAgent` full rebuild | ≤ 1 full rebuild / 100 ms under token flood |
| **Cancel to first non-running frame** | operator trust | ≤ 100 ms after Ctrl-X with scripted provider |
| **Long-session soak** | 50 turns × tool bodies | no frame > 33 ms drain+rebuild; no UI block > 8 ms |

`make test-perf` **must not** become a portable microbenchmark. Keep wide algorithmic budgets; add **structural** probes that fail on O(n²) / UI-thread I/O regressions.

### 2.3 What the gates already prove (do not regress)

- Parser is linear across split tags + compact at 64 KiB (`parser.cpp:31–78`)  
- One view rebuild per drained batch (`batchingEvents` / `viewRebuildPending`)  
- Bridge does not retain Token/Protocol in snapshot (`agent_bridge.hpp:96–101`)  
- Wrap cache hits when `transcriptVersion` + width stable (`chat_view.hpp:446–481`)

---

## 3. Paradigms (good vs anti) + recommended shifts

### 3.1 Good paradigms (preserve)

| Paradigm | Where | Why it works |
|----------|-------|--------------|
| **Streaming-linear parser** | `parser.cpp` feed / compact / suffix hold | No whole-buffer rescans; 64 KiB compact |
| **Dirty protocol index diff** | `protocol_event_diff.hpp` | O(changed slots), RETRY-safe |
| **Publish coalesce + batch lock** | `mk3_tui_app.hpp` 16 ms + `publishMany` | One mutex / one eventfd wake per flush |
| **Tick-coalesced drain (not on_wake)** | `installAppTick` comment 63–69 | Prevents 50–100 rebuilds/sec wake-storm |
| **Batch then single rebuild** | `drain` 1268–1279 | Correct batching primitive |
| **Versioned wrap cache + dirty tail** | `transcript_cache.hpp`, `chat_view.hpp` | Avoids O(n²) rewrap during stream |
| **Body / sanitize caps** | 16 KiB sanitize, 8 KiB row body, 50 body lines | Hard limits on paint cost |
| **Snapshot-bounded bridge history** | Token/Protocol excluded; snapshot ring 128 | Memory stays bounded |
| **inkcell surface swap + diff_frame** | `engine.hpp` 266–274 | O(1) buffer swap; differential ANSI |
| **Nested refresh rate limit** | 100 ms | Stops per-token `rowsFromAgent` |

### 3.2 Anti-paradigms (active debt)

| Anti-pattern | Evidence | Effect |
|--------------|----------|--------|
| **Full rebuild culture** | `rebuildViews` rebuilds **all** display lines; called from apply path, selection, toggles, persist | Amortized cost ∝ session length |
| **`vector::erase` from front for cap** | `enforceRowCap` 626 | O(n) memmove of `TimelineRow` (strings) |
| **UI-thread disk I/O** | `persistUiTimeline` in `apply(TurnDone)` | End-of-turn freezes |
| **Pretty-print every save** | `manager.cpp:172` `stringify(..., pretty=true)` | Multi-MB sessions hurt |
| **Load-merge-save on every timeline snapshot** | `persistUiTimeline` 1360–1364 | Double parse of session JSON |
| **God-model owns policy + I/O + reduce** | `ShellModel` ~1k methods | Cannot unit-budget pure steps |
| **Global cancel flag** | `g_running` | Works but couples layers; multi-agent cancel is process-wide |
| **Engine Ctrl-C = hard quit** | `engine.hpp:314–318` before `on_key` | Scene “cancel turn” on Ctrl-C is dead; Ctrl-X is real stop |
| **Dual TUI stacks** | `src/tui` vs `src/ui` | Two poll policies, two history models |
| **String-keyed semantic headers in wrap** | `wrapTranscriptRange` probes `"YOU"`, `"CORTEX"`, … | Fragile + extra per-line work; should be kind enum from rebuild |

### 3.3 Recommended paradigm shifts

1. **Projection, not rebuild**  
   `EventReducer` mutates a row store; `ViewProjection` incremental-updates only dirty row ranges into `transcriptView.lines`. `transcriptVersion` bumps per dirty range, not full clear.

2. **Ring / deque row store**  
   Cap drops from the front in O(1) or O(k) without memmoving the protected tail. Or store rows in a chunked deque and rebuild index maps lazily.

3. **Session commits as async jobs**  
   `SessionController.commitAsync(Snapshot)` — UI thread copies POD snapshot (or takes ownership of a swapped string), worker serializes + single-writer save. Coalesce generations so rapid TurnDone + atexit collapse to one write.

4. **Latency budgets as product contract**  
   Explicit: parse ≤ X, drain ≤ Y, wrap ≤ Z, draw ≤ W, **persist always async**. Publish in docs next to `CORTEX_PERFORMANCE.md`.

5. **One cancel token per turn**  
   Injected into provider/parser; Ctrl-X / palette stop set it; Engine Ctrl-C policy decided once (quit app vs cancel turn) — do not leave dead scene handlers.

6. **Reuse modular extractions** (modularity audit P1.1–P1.2)  
   - `TimelineCodec` — serialize/deserialize only  
   - `RowPolicy` — sanitize, body cap, enforceCap (pluggable store)  
   - `EventReducer` — pure `apply(UiEvent) → mutations`  
   These are the **reusability** path for a second UI and for headless perf harnesses.

---

## 4. Hotspots severity table (file:line)

| Sev | Location | Mechanism | Symptom |
|-----|----------|-----------|---------|
| **P0** | `inkcell_app_model.hpp:1228, 1342–1366` `persistUiTimeline` | Serialize + load + pretty save on UI thread at TurnDone | “Model finished → TUI freezes” on long runs |
| **P0** | `session/manager.cpp:171–174` | `stringify(root, pretty=true)` every save | Multi-MB sessions amplify TurnDone hang |
| **P0** | `inkcell_app_model.hpp:608–633` `enforceRowCap` | `rootRows.erase(begin, begin+drop)` | Hitch when cap bites mid-stream |
| **P1** | `inkcell_app_model.hpp:708–886` `rebuildViews` | Full re-emit of all labels + body lines | Cost grows with session; thrash wrap cache source |
| **P1** | `inkcell_app_model.hpp:1192–1193, 682` | `rebuildViews` / `applyRowBans` from hot apply | Even with batching, end-of-batch work is full O(n) |
| **P1** | `inkcell_app_model.hpp:979–997` `refreshNested` + `rowsFromAgent:325+` | Full protocol→row rebuild of child | Nested drill jank if rate limit regresses |
| **P1** | `mk3_tui_app.hpp:158–159, 276–277` dual flush | Two `saveSession` paths + timeline | Exit latency + session audit F1/F2 |
| **P1** | `engine.hpp:314–318` vs `agent_scene.hpp:103–110` | Engine quits on Ctrl-C; scene stop unreachable | Operator cancel UX inconsistent (Ctrl-X vs Ctrl-C) |
| **P1** | `mk3_tui_app.hpp:207–214` protocol dirty scan | Full vector compare when sizes equal | Extra O(events) on worker each token (usually small; grows with long protocol lists) |
| **P2** | `inkcell_app_model.hpp:81–115` sanitize | Per-row single-pass (OK) but allocates new string always | Alloc pressure under raw mode floods |
| **P2** | `inkcell_app_model.hpp:635–682` dual body cap | 8 KiB row then 16 KiB sanitize | Redundant copies for large tool bodies |
| **P2** | `chat_view.hpp:336–394` wrap | Word wrap + UTF-8 width per dirty line | Fine with cache; full rewrap on resize costly |
| **P2** | `chat_view.hpp:369–375` semantic header probes | String prefix checks per source line | Should use TimelineKind from model |
| **P2** | `agent_bridge.hpp:99–100` snapshot erase front | O(n) on non-stream events past 128 | Rare; ring buffer would be cleaner |
| **P2** | `repl_session.hpp:735–748` | Separate 2/50 ms poll | Dual stack maintenance |
| **P2** | `inkcell_app_model.hpp:1457–1458` first-submit `saveSession` | Sync disk on first Enter | First-prompt latency spike |
| **P3** | `ensureSelectionVisible` 888–905 | Re-walks rows + `splitDisplayLines` | Selection jank on huge bodies |
| **P3** | Inspector rebuild inside `rebuildViews` | Always rebuilds inspector lines | Cheap but unnecessary every protocol tick |

### 4.1 Severity rubric

- **P0** — User-visible freeze or correctness-adjacent stall under normal long chat  
- **P1** — Measurable jank / wrong cancel semantics / scales badly  
- **P2** — Waste, dual-stack, or future scale risk  
- **P3** — Polish / micro

---

## 5. UX latency perception matrix

Operators do not feel “parser ms.” They feel **time-to-first-paint**, **scroll stickiness**, **Esc/cancel**, **turn-done unlock**, **hub enter**.

| Operator action | Expected feel | Current binding | Risk under semi-long run |
|-----------------|---------------|-----------------|--------------------------|
| First token paint | < 100 ms after TTFT | 16 ms publish + ≤33 ms tick | Low (Cortex); TTFT free-tier high |
| Streaming scroll stick | Continuous bottom follow | `stick_bottom` + rebuild | Medium — rebuild cost grows |
| Esc / timeline focus | Instant | `focusTimeline` → full `rebuildViews` | Medium at 600 rows |
| j/k block nav | Instant | `selectDelta` → `rebuildViews` | Medium (full rebuild for selection glyph) |
| Ctrl-X stop | Immediate “cancelling…” | `g_running=false` + notice | Medium — wait for provider/loop |
| Ctrl-C | Cancel turn (expected) | **Engine quits app** | **High** — wrong mental model |
| Turn complete → type next | Immediate composer | TurnDone → **persist blocks** | **High** on large ui_timeline |
| Drill into sub-agent | < 100 ms | `rowsFromAgent` full rebuild | Medium on fat children |
| Live nested while child streams | Smooth | 100 ms refresh gate | OK if gate holds |
| Huge tool result lands | Truncated, no freeze | 8 KiB + sanitize + 50 lines | Low if caps not bypassed |
| Toggle thoughts/raw | Instant | rebuild + prefs write | Low–medium |
| Hub Sessions list | Instant | directory `list()` | Low at dozens; weak at hundreds |
| Resume session | Fast paint | `loadSessionUi` preferred | Medium if pretty multi-MB parse |
| Quit / SIGINT | No data loss, fast exit | atexit + dual flush + timeline | Medium — double write |

**Perception rule:** any UI-thread pause > **~16–33 ms** is a “stutter”; > **~100 ms** is a “hang.” Persist and cap erase are the hang class.

---

## 6. Live semi-long run correlation

**Setup:** operator semi-long multi-turn chat on `deepseek-v4-flash-free` (slow TTFT, bursty tokens, tool-heavy turns).

| Observed class | Code correlation | Notes |
|----------------|------------------|-------|
| Slow start of answer | Provider TTFT, not Cortex | Free-tier; Cortex paint path is sub-frame after first bytes |
| Smooth early streaming | 16 ms publish + tick drain + wrap cache | Matches design |
| Growing sluggishness late session | `rebuildViews` O(rows) + wrap source snapshot copy | Cap at 600 delays but does not remove rebuild culture |
| Occasional hitch mid-run | `enforceRowCap` erase + protocol flood batches | Especially with raw mode / many Stream rows |
| Freeze at end of heavy turn | `persistUiTimeline` + pretty `save` | Session audit F2/F10 couple here |
| Cancel unreliability feel | Ctrl-C quits; Ctrl-X cancels; stream may drain late | Engine policy + `g_running` poll points |
| Nested sub-agent “catch-up” | `refreshNested` 100 ms + full `rowsFromAgent` | Better than per-token; still full rebuild |
| Resume quality variance | `ui_timeline` vs records-only | Session audit F3/F4 — not pure perf but feels like “slow wrong paint” |
| Exit slow / double write | dual-id flush | Session audit F1 |

**Thesis confirmed by live run:** free-model latency is mostly **provider + structural jank**, not parser. Fixing pretty-save + async commit + cap store would move the needle more than another parser micro-pass.

---

## 7. P0 / P1 / P2 backlog + verify methods + targets

### P0 — stop freezes (this week)

| ID | Work | Target | Verify |
|----|------|--------|--------|
| **Perf0.1** | **Async session commit** — snapshot ui_timeline (+ records if needed) off UI thread; generation coalesce; single writer mutex | UI thread ≤ **8 ms** after TurnDone; disk may lag | New gate: inject 2k-row timeline TurnDone; assert draw-thread wall < 8 ms; file eventually consistent |
| **Perf0.2** | **Compact save path** for hot commits (or always compact; pretty only for export) | Save CPU ≤ ½ of pretty for multi-MB | Unit: stringify compact vs pretty ratio; soak 50 turns |
| **Perf0.3** | **O(1)/deque cap store** — replace front `vector::erase` | Cap drop 100 rows ≤ **2 ms** | New `test-perf` cap hitch gate |
| **Perf0.4** | **Cancel UX fix** — either Engine delivers Ctrl-C to scene when turn running, or document Ctrl-X as stop and make chrome say so; ensure stop does not wait on persist | Cancel notice ≤ 1 frame; agent stops ≤ 100 ms (scripted) | ScriptedProvider long stream + Ctrl-X |

Joint with session audit: **Perf0.1 ≡ SessionController.commitAsync** (session S0.2 write lock + S2.1 size policy).

### P1 — amortized frame cost (next)

| ID | Work | Target | Verify |
|----|------|--------|--------|
| **Perf1.1** | Extract **EventReducer** + incremental projection (dirty rows only) | `rebuildViews` full path rare; steady drain ≤ **4 ms** @ 600 rows | Extend reducer gate with 600 seeded rows + 100 mutations |
| **Perf1.2** | Extract **RowPolicy** (sanitize, body cap, enforceCap) pure | Unit-testable without Surface/Agent | `row_policy_test` + perf cap gate |
| **Perf1.3** | Extract **TimelineCodec** | Serialize 2k rows ≤ **15 ms**; no I/O in codec | `timeline_codec_test` + timing |
| **Perf1.4** | Selection without full rebuild (toggle `›` on two lines only) | j/k ≤ **1 ms** model work | Micro harness |
| **Perf1.5** | Nested: incremental child projection or reuse reducer | Nested refresh ≤ **4 ms** typical | Nested flood test with 100 ms gate assertion |
| **Perf1.6** | Worker protocol dirty: track generation counter / last-changed index instead of full scan when possible | onToken overhead flat | Instrument worker under long protocol lists |

### P2 — scale & hygiene

| ID | Work | Target | Verify |
|----|------|--------|--------|
| **Perf2.1** | Ring snapshot for bridge non-stream events | No front erase | Code review + asan |
| **Perf2.2** | Kind-enum to wrap path (drop string semantic probes) | Cleaner + slightly faster wrap | Wrap cache tests |
| **Perf2.3** | Quarantine / freeze `src/tui` perf work | One product path | Build flag / docs |
| **Perf2.4** | ui_timeline size budget (drop Stream/Thought from disk projection) | File growth linear in durable turns | Session size soak |
| **Perf2.5** | Long-session soak job (50–100 turns scripted) | No frame drain+rebuild > 33 ms p99 | CI optional job |
| **Perf2.6** | First-submit save async (arm id sync, disk async) | First Enter feels instant | Manual + timing |

### Numeric product targets (publish beside gates)

| Metric | Target |
|--------|-------:|
| Parser 256 KiB 1-byte | ≤ 250 ms (existing) |
| Drain 1000 protocol / batch | ≤ 100 ms (existing); aspire ≤ 10 ms |
| Cached wrap 3000×200 | ≤ 150 ms (existing) |
| UI thread max block | **8 ms** |
| TurnDone → composer interactive | **≤ 16 ms** (persist async) |
| Cap enforcement | **≤ 2 ms** |
| Cancel (Ctrl-X) to status update | **≤ 33 ms** (1 frame) |
| Steady rebuild @ 600 rows | **≤ 8 ms** (until incremental projection) |

---

## 8. What NOT to optimize

| Temptation | Why not |
|------------|---------|
| **Parser micro-opts** | Already 40–400×; gates green with huge margin (7 ms vs 250 ms) |
| **inkcell differential renderer rewrites** | Deferred by design (`CORTEX_PERFORMANCE.md`); needs inkcell bench suite |
| **Engine dirty-rect / Surface alloc** | inkcell boundary; Cortex should feed less, not fork renderer |
| **Key decoding completeness** | Not a throughput problem |
| **SGR / theme tweaks for “speed”** | Perception ≠ paint cost |
| **Provider TTFT** | External; free-tier noise |
| **Premature SIMD / custom JSON** | Compact pretty-flag + async commit first |
| **More aggressive publish than 16 ms** | Gains invisible under 33 ms UI tick; risks wake storms if on_wake drain returns |
| **Removing body caps** | Caps are load-bearing anti-freeze; expand only with virtualization |
| **Unifying stacks by porting ReplSession hacks into inkcell** | Extract pure modules; deprecate legacy |

---

## 9. Coupling to session audit (async commit)

Session audit findings that **are** performance findings:

| Session ID | Perf coupling |
|------------|---------------|
| **F1** dual session id / double flush | Exit latency ×2; wasted JSON |
| **F2** concurrent writers, no mutex | Data race risk + retry/tearing under async |
| **F10** pretty JSON every save | Dominant TurnDone cost |
| **S2.1** ui_timeline size budget | Disk + serialize time |
| **S0.2** write lock | Required before true async commit |
| **S0.1** single active id | Required so async worker has one target |

**Joint fix (both audits):**

```text
SessionController
  · one active SessionRef
  · commitAsync(Snapshot{records?, uiTimeline, meta, generation})
  · process-local write mutex
  · compact JSON hot path
  · coalesce: newer generation supersedes in-flight if not yet writing
```

UI rule: **`apply(TurnDone)` never calls `load`/`save`.** It enqueues a snapshot. atexit waits for outstanding commit (with timeout) or falls back to sync once.

Modularity audit maps:

| Modularity | Perf |
|------------|------|
| P0.4 SessionController | implements Perf0.1–0.2 |
| P1.1 TimelineCodec + RowPolicy | Perf0.3, Perf1.2–1.3 |
| P1.2 EventReducer | Perf1.1 |

---

## 10. Sequencing

```text
Phase A — Stop freezes (1–2 days)
  1. Compact save (or hot-path compact)                         Perf0.2
  2. Session write mutex + single active id hooks               Session S0.1–S0.2
  3. commitAsync skeleton: TurnDone enqueues, worker writes     Perf0.1
  4. Cap store without front erase (deque or mark+compact)      Perf0.3
  5. Extend test-perf: cap hitch + non-blocking TurnDone probe

Phase B — Amortize frames (2–4 days)
  6. Extract TimelineCodec / RowPolicy / EventReducer           Mod P1.1–P1.2
  7. Incremental projection (dirty rows)                        Perf1.1 / 1.4
  8. Nested reuse reducer                                       Perf1.5
  9. Cancel policy (Engine Ctrl-C vs Ctrl-X) documented/fixed   Perf0.4

Phase C — Productize (ongoing)
  10. ui_timeline durable subset (drop ephemeral kinds)         Perf2.4
  11. Soak job + publish latency budget table in CORTEX_PERFORMANCE.md
  12. Freeze legacy tui perf work                               Perf2.3
```

**Do not** start with hub polish, inkcell renderer rewrites, or parser “one more pass.”

Dependency order: **mutex + single id → async commit → cap store → reducer extraction → incremental projection.**

---

## References

| Path | Why |
|------|-----|
| `docs/CORTEX_PERFORMANCE.md` | Historical wins + permanent gate table |
| `src/testing/perf_test.cpp` | Living gate implementation |
| `src/protocol/parser.cpp:31–78` | feed / compact design |
| `src/ui/model/protocol_event_diff.hpp` | Pure dirty diff + RETRY contract |
| `src/ui/bridge/agent_bridge.hpp` | Queue, eventfd, snapshot bounds |
| `src/ui/app/mk3_tui_app.hpp:62–85, 201–248` | Tick coalesce + 16 ms publish |
| `src/ui/model/inkcell_app_model.hpp:81–115, 607–682, 708–886, 979–997, 1030–1281, 1342–1366` | sanitize, cap, rebuild, nested, apply/drain, persist |
| `src/ui/chat/transcript_cache.hpp` | Versioned wrap state |
| `src/ui/chat/chat_view.hpp:336–555` | wrap range + drawTranscript incremental |
| `src/ui/scenes/agent_scene.hpp:103–126, 297–376, 624–636` | cancel keys, draw, stopAgentLoop |
| `src/tui/repl_session.hpp:735–748` | Legacy poll profile |
| `src/session/manager.cpp:128–175` | pretty save |
| `../inkcell/include/inkcell/engine.hpp:70–110, 220–345` | tick, wake_fd, select, Ctrl-C, diff_frame |
| Session audit | dual-writer, pretty, joint commitAsync |
| Modularity audit | TimelineCodec / RowPolicy / EventReducer extraction plan |

---

### Reusability note (perf × modularity)

Today only **parser**, **protocol_event_diff**, **transcript_cache**, and (partially) **AgentBridge** are headless-callable with clear budgets. Long-run perf work that lands **inside** `ShellModel` without extracting `TimelineCodec` / `RowPolicy` / `EventReducer` will:

1. remain untestable without inkcell + Agent  
2. block a second UI / automation client from inheriting the same latency contract  
3. re-create dual-stack drift with `src/tui`

Treat extractions as **performance infrastructure**, not cleanup vanity.

---

*Audit status: complete, code-authoritative. Implementation not started in this pass. `make test-perf` verified green (7 / 48 / 2 / 43 ms vs budgets 250 / 300 / 100 / 150).*
