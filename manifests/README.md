# Standard Manifest Library (PROD)

**Rule of thumb**

| Tree | Role |
|------|------|
| `manifests/` | **PROD / std global registry** — auto-discovered by the hub, name-resolvable |
| `config/` | **DEV / MVP / experiments** — not auto-hubbed; promote into `manifests/` when battle-tested |

Inkcell is a separate terminal primitive kit. Cortex UI chrome lives in `src/ui/{assets,components}/`.

## Layout

```
manifests/
  agents/           PROD agents (default, coder + specialists, …)
  built-in/tools/   compiled tool manifests
  built-in/feeds/   compiled feed manifests
  workflows/        workflow definitions (renderer landing soon)
  harness/          harness size profiles
  prompts/          reusable prompt modules
  skills/           operating skills
  persona/ system/  shared context defaults
```

## Agent standard (MVP → PROD)

Every PROD agent directory:

```
agents/<name>/
  agent.yml          kind/name/version/summary + cognitive_engine + runtime + import
  system.md          (or context.system path)
  persona.md         optional
  README.md          purpose, tools, sub-agents, launch line
  agents/<child>/    nested specialists (optional)
```

**Required `runtime:` keys (MVP):**

```yaml
runtime:
  max_iterations: N
  history_cap: N
  mode: normal            # normal | autonomous
  # dev_mode: true        # live dumps → ~/.cortex/dev/<session>/
  subagents:
    persistence: session  # when children exist
```

## Dashboard hub

`a` / Manifests section recursively lists **all** PROD manifests (agents, tools, feeds, workflows, harness, prompts, skills).  
`f` cycles kind filter. Enter on a launchable agent shows the CLI relaunch hint.

## Promotion path

1. Prototype under `config/agents/…` (or `config/…`)
2. Stabilize contracts + README
3. Move into `manifests/` and register in `CATALOG.md`
4. Never leave broken duplicates in PROD (name collisions kill trust)
