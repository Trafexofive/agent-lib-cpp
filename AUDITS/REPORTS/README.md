# AUDITS / REPORTS

Operator-facing engineering audits. Dated, evidence-backed, actionable.

| Report | Focus |
|--------|--------|
| [2026-07-26-session-management-audit.md](./2026-07-26-session-management-audit.md) | Session identity, durability, resume/fork, hub UX, dual-store architecture |
| [2026-07-26-speed-performance-audit.md](./2026-07-26-speed-performance-audit.md) | Hot path, jank, paradigms, UX latency, perf backlog |
| [2026-07-26-code-quality-modularity-audit.md](./2026-07-26-code-quality-modularity-audit.md) | Quality, readability, modularity, **reusability**, extraction map, layering |

Supporting discovery artifacts (session):  
`.artifacts/session-management-evidence-map`,  
`.artifacts/speed-performance-evidence-map`,  
`.artifacts/session-perf-crosscut-analysis`,  
`.artifacts/modularity-audit-evidence-pack`,  
`.artifacts/modularity-audit-agent-lib-cpp`.

## Dual-repo integration (inkcell + agent-lib)

Living contract: [`docs/INKCELL_INTEGRATION.md`](../../docs/INKCELL_INTEGRATION.md)  
First-product review: [`docs/tui-qol/00-inkcell-first-product-review.md`](../../docs/tui-qol/00-inkcell-first-product-review.md)  
Session artifact: `inkcell-agentlib-dual-repo-ledger` (`art-ms307ws7-5adh5b`).
