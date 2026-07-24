# Operator Notes — CleverLord / Cortex-Prime MK3

Durable session notes. Survives compaction. Update when prefs crystallize.

---

## Code bar
- C++11 default unless real reason newer
- Production-grade: no stubs, no lazy patches, no overengineering
- Readable modular files over clever layers
- Explicit git stage only — never `git add -A` (dirty tree is hostile)
- Makefile header-deps broken → force-rebuild objects after header edits
- Gate: `make test-ui-view test-ui-model test-chat-scene test-perf parser-test`

## UX canon (locked)
- Sub-agent RESULT = final child text only; no nested child blocks in parent
- Manual ↳ Enter into child chat; Esc back; **no auto-enter/leave**
- Nested chat = same palette/primitives as parent (protocolEvents + child agentName)
- Non-ephemeral sub-agent = continuous history across parent re-prompts
- Parent vs User labeled in child history; live `[FROM parent agent "…"]`
- Agent ops: `op=prompt|inspect|context|history`, `ephemeral`, `dump_context`
- Empty/whitespace thoughts never render
- Thoughts + truncation first-class (defaults ON)
- Multi-turn: every `"running"` status resets protocol-mapping epoch
- Stream ≈ parse: thought live, action card on open tag, protocol flush unthrottled
- Animations only with explicit permission
- Main menu still ~3.5/10 — **parked** until askcards/builtins/manifest-expert ship

## CLI lifecycle (orthogonal)
| Flag | Meaning |
|------|---------|
| `-p` / `--prompt` | seed prompt only |
| `--no-session` | no session load/save |
| `--ephemeral` | exit on turn done |
| Manifest `runtime.no_session` / `runtime.ephemeral` | OR with CLI |

## Harness
- CANON wins: `docs/protocol/CANON.md`
- Projections: `manifests/harness/{small,medium,default,big}.md`
- Teach full surface + nuance, not generic agent vibes

## Observations from live sessions
1. Nested Result blocks rejected — leakage look
2. Auto-enter sub-agent rejected — operator control
3. Header-only RESULT rejected — final text required
4. Speculative loop escapes = broken; baseline restore
5. Empty THOUGHT headers = noise
6. Multi-turn clobber: running pre-set + protocol index remap
7. Sub-agent history: keep protocolEvents on continuation; rowsFromAgent full timeline
8. Nested monotone: classify with **child** agentName; prefer structured events
9. Dashboard Agents/Sessions/Harness pass insufficient — redesign later
10. Stream: provisional action + live thought + dirty flush — live-validate
11. Billing 402 on live-smoke is upstream, not always local regression
12. **Empty-response RETRY path must not pad-then-clear protocol baseline** — OOB write → tcache/SEGV (fixed: `protocol_event_diff.hpp`)
13. Bare TUI: no phantom session mint; first submit arms id + seeds User line + save
14. Backspace/Esc: drilldown pop first; chat-root → main; empty composer Backspace → main
15. Huge tool/manifest row bodies: hard-cap before wrap (8KiB) + sanitize byte-cap

## Gate (current)
```
make test-protocol-event-diff test-chat-scene test-ui-model test-lazy-session cortex-mk3
# full before "done": + test-ui-view test-perf parser-test
```
Header deps weak → `find . -name '*.o' -delete` after header edits.

## Next track
1. Live-confirm RETRY path: `./cortex-mk3 -m manifests/agents/brainstormer/agent.yml --tui experimental` + "ping a subagent" survives retries
2. Resume path: recover session shows typed User line + any Agent content
3. Backlog (from noise-bleed handoff): remote tools/relics, workflow renderer v1, select-mode G/gg, session name/notes, trigger surface on agent manifests
4. Optional: Agent-side mutex around history_/protocolEvents_ if races remain after OOB fix

## Handoff artifacts
- **Active:** `session-handoff-retry-oob-heap-fix` (this crash class)
- **Backlog/context:** `session-handoff-noise-bleed-to-runtime`
- **Archive:** `session-handoff-pre-compact-askcards-manifest` (historical)
- **Design park:** `compaction-manifest-drafts-v0`

## Commits (crash-fix arc tip)
`7bc8f97` RETRY OOB · modular `protocol_event_diff` + regression test · `2ffba37` row body cap · `ea82265` sanitize simplify · `3f960c7` seedUserPrompt · `e01b96c` load-backfill

*The Great Work Continues… — GODSPEED.*
