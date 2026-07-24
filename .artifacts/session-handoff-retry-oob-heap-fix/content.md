# Session Handoff — RETRY OOB heap fix + artifact audit

**Date:** 2026-07-24  
**Branch:** `feat/inkcell-agentshell`  
**HEAD (pre-this-commit tip):** `7bc8f97` then modular extract  
**Binary:** rebuild `cortex-mk3` after header edits (`find . -name '*.o' -delete`)

---

## 1. Root cause (this was the real crash)

Operator live path:
```
ping a subagent → empty response → RETRY retry N/M → tcache_thread_shutdown /
unaligned tcache / SEGV
```

Bug in `collectProtocolChanges` (was inline in `mk3_tui_app.hpp`):

```cpp
previous.resize(current.size());   // pad
if (rotatedAtZero) previous.clear();
for (i…) previous[i] = current[i]; // OOB when previous.size()==0
```

Pure out-of-bounds `std::vector` write. Only fires on empty-response retry
(agent clears `protocolEvents_`, pushes `RETRY` marker). Matches every
"I didn't do anything / just pinged a subagent" dump.

### Fix (modular)
| Path | Role |
|------|------|
| `src/ui/model/protocol_event_diff.hpp` | pure `sameProtocolEvent` + `collectProtocolChanges` |
| `src/ui/app/mk3_tui_app.hpp` | includes the header (no duplicate body) |
| `src/testing/protocol_event_diff_test.cpp` | 13 assertions, no network |
| `make test-protocol-event-diff` | regression gate |

Correct sequence: detect RETRY → clear → truncate-only (never pad) → assign or `push_back`.

---

## 2. Related session/UX commits still load-bearing

```
7bc8f97 fix(ui): OOB write in collectProtocolChanges on RETRY rotation
2ffba37 fix(chat): hard-cap row bodies at 8KiB before wrap and rebuild
ea82265 fix(sanitize): single-pass byte cap
3f960c7 fix(session): seedUserPrompt on submit so early exit keeps User record
e01b96c load-backfill empty agent_name + session-test NoopProvider
c624764 drilldown-aware Backspace/Esc; lazy session arm; 30s stream cap
14ed6dc atexit session flush
```

---

## 3. Artifact audit (project `.artifacts/`)

| Artifact | Age (approx) | Status | Action |
|----------|--------------|--------|--------|
| `operator-notes-cleverlord` | Jul 20 | **Partially stale** | UX canon still valid; "Next track askcards/manifest-expert" and commit tip list are old. Update after this turn. |
| `session-handoff-noise-bleed-to-runtime` | Jul 23 | **Stale cursor** | Points at `e67ca36`; bleed/retry notification work is landed; backlog still useful. Supersede cursor with this handoff. |
| `session-handoff-pre-compact-askcards-manifest` | Mar 27 / Jul 20 | **Historical** | Keep as archive; do not treat as active plan. |
| `compaction-manifest-drafts-v0` | Jul 20 | **Still design-only** | "Not implemented" — leave until compaction work starts. |
| `drilldown-hang-bug-analysis` | Jul 18 | **Likely fixed** | Verify against current `enterSelected`/`goBack`; archive if green. |
| `inkcell-*` audits | Jul 12 | **Reference** | inkcell HEAD still `feb1fa6` territory; re-audit only if renderer/input regresses. |
| `session-handoff-hub-to-workflows` | Jul 22 | **Backlog** | Workflows lane still open. |
| `cortex-pi-level-gap-analysis` | Jul 17 | **Strategic** | Keep; not day-to-day execution. |
| `parent-subagent-delegation-report` | Jul 18 | **Reference** | Subagent continuity rules still in operator-notes. |

### Operator-notes deltas to apply
- Esc ladder + Backspace: drilldown pop then main; composer empty Backspace = main (not two-Esc only).
- Session: bare launch no mint; first submit arms id + seeds User line + save.
- Crash class: empty-response RETRY OOB is fixed; do not reintroduce pad-then-clear-then-index.
- Gate add: `make test-protocol-event-diff` next to chat/ui-model.

---

## 4. Live verify (operator)

```bash
find . -name '*.o' -delete; find build -type f -delete
make test-protocol-event-diff test-chat-scene test-ui-model cortex-mk3
./cortex-mk3 -m manifests/agents/brainstormer/agent.yml --tui experimental
# "ping a subagent" — must survive empty-response retries without IOT/SEGV
# recover session — must show at least the typed User line
```

Upstream 402/403 billing still not local regression.

---

## 5. Intentionally not done this slice

- Full Agent mutex around history_/protocolEvents_ (bridge is already locked; OOB was the smoking gun).
- Trigger surface on agent manifests (still open design).
- Workflow renderer v1.
- Main menu redesign.
- ASAN CI job (worth adding later; pure unit test covers the known OOB).

---

*The Great Work Continues… — GODSPEED.*
