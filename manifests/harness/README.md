# Harness prompts

Model-facing projection of `docs/protocol/CANON.md`, plus a **capability model** so the LLM understands what the runtime can *do* — not only what syntax is legal.

| File | Role | ~tokens | Intent |
|------|------|---------|--------|
| `small.md` | Minimal contract + power bullets | ~0.3–0.4K | Sub-agents, tight windows, strong models |
| `medium.md` | **Default** — capabilities + rules | ~1.2–1.4K | Daily driver |
| `big.md` | Full power model + patterns + examples | ~2.3–2.6K | Weaker models, compliance tuning, first exposure |
| `default.md` | Alias of **medium** | same | Stable path when manifests omit `context.harness` |

## What “deeper understanding” means here

All sizes teach (at different density):

1. **Harness as control plane** — tags drive a real executor, not formatting cosplay  
2. **Orthogonal surfaces** — tool / agent / feed / relic / workflow  
3. **Parallelism as default posture** — fan-out in one generation  
4. **Piping** — `${id}` / `depends_on` as dataflow edges  
5. **Loop physics** — act → `<result status>` → continue; no premature `final`  
6. **Composition patterns** — fan-in, delegate, ambient, persist, verify  

`big` adds worked examples and a longer pattern catalog. `small` keeps the same truths in bullets.

## Select

**Agent manifest:**
```yaml
context:
  harness: ../../harness/small.md   # or medium.md / big.md / default.md
```

**CLI:**
```bash
cortex-mk3 -m default --harness small
cortex-mk3 -m default --harness medium
cortex-mk3 -m default --harness big
```

Bare names resolve under global `manifests/harness/`.

## Tone

- Calm, dense, power-oriented — not intimidation banners  
- No ❌ failed-phrase lists  
- Result wire format matches runtime: `status="ok|error|timeout|protocol_error"`  
- If harness disagrees with CANON, **CANON wins**
