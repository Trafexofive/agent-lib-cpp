# ctx/ — shared engagement state

Bound into every agent in the fleet at `/workspace/ctx` (writable).

- `engagement.md` — the ledger: scope, authorization, decision log, findings.
- `findings-report.md` — final parent deliverable (created per engagement).
- Anything else written here is campaign state (notes, PoCs, tool output).

This directory is the C01 endeavor-state pattern: no relic required for v1.
It is the ONLY coupling between the parent and its isolated children.
