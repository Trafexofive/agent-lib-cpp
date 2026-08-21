# live-lab operator script

Run these in the TUI after `cortex-mk3 -m live-lab` (restart binary first).

1. `present yourself` — parent final; no fake child.
2. `ask me one confirm card: proceed y/n` — ask_tool, then honor results.
3. `ping echo-worker and probe-worker in one generation, both present themselves` — two AGENT cards visible **before** either RESULT.
4. `ping echo-worker twice async ephemeral, present themselves` — `#id-a` and `#id-b` both open; header shows `async eph`.
5. After a result: drill ↳ into a child — well shows provider/model + iter.
6. Ctrl-X mid-child — parent stops; sibling must not resurrect the tree.
7. Optional: kill primary (bad key / stall) — harness FALLBACK → minimax-m3, next user turn back on deepseek-v4-pro.

Pass = cards + footer + harness match the dump, not just the final prose.
