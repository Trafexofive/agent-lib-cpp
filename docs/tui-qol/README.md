# TUI QoL + Animation Detour

**Status:** planning docs only — no code until you pick a pack.  
**Branch context:** `feat/inkcell-agentshell` after SessionController / Pi keys / viewport / sanitize.  
**Source audits:** `chat-ux-audit-report.md`, `AUDITS/REPORTS/*`, live operator feedback.

## Why this detour

Product path is functional. Feel is still “works but amateur” in a few always-on surfaces:

- chrome identity (header / status / mode)
- graphite contrast + selection
- nested drill chrome lying about metrics
- composer line-editing paper cuts
- cheap motion that sells “alive” without jank

**Non-goals for this detour:** async LLM, multi-line composer redesign, ask_cards DAG, full virtualization rewrite (already have span/viewport path), hard-delete legacy TUI.

## Integration context (read first after compact)

- [00-inkcell-first-product-review.md](./00-inkcell-first-product-review.md) — what Cortex does well/worst on inkcell
- [../INKCELL_INTEGRATION.md](../INKCELL_INTEGRATION.md) — dual-repo doctrine + backlog
- Artifact: `inkcell-agentlib-dual-repo-ledger`

## Packs (pick one or compose)

| Pack | File | Effort | ROI |
|------|------|--------|-----|
| **Feel** (recommended first) | [01-feel-pack.md](./01-feel-pack.md) | 2–3h | Highest — every session |
| **Composer** | [02-composer-pack.md](./02-composer-pack.md) | 1–2h | Daily friction |
| **Nested drill** | [03-nested-drill-pack.md](./03-nested-drill-pack.md) | 2–3h | Core differentiator |
| **Full QoL day** | [04-full-qol-day.md](./04-full-qol-day.md) | ~1 day | Feel + composer + nested |
| **Animation budget** | [05-animation-budget.md](./05-animation-budget.md) | — | Intensity policy |
| **Item catalog** | [06-item-catalog.md](./06-item-catalog.md) | — | All pickable items mapped to files |

## Already shipped (don’t re-do)

- Braille live spinner + blink cursor (`chat_view.hpp`)
- Field shaders / pill slide / cmd palette scale-fade / workflow pulse
- Incremental projection + viewport span virtualization
- Binary/UTF-8 sanitize on Response + upsert + resume
- Pi Ctrl-C / Esc semantics

## Verify bar for any pack

```bash
make cortex-mk3 test-chat-scene test-ui-model test-ui-view -j$(nproc)
# manual: graphite + neon, idle + running, nested drill enter/back
```

## Decision

Standby after these docs. Tell the agent which pack (or item list) to implement.
