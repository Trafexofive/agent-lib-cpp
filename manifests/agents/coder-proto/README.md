# coder-proto

> Implementation coordinator for Cortex-Prime MK3 (prototype roster).  
> **Parent writes.** Specialists: **discovery** (map) + **reader** (task evidence).  
> Evidence-first → smallest correct diff → verify → report.

## Tree

```text
coder-proto/
├── agent.yml
├── system.md
├── persona.md
└── agents/
    ├── discovery/    area map (structure · conventions · risk)
    └── reader/       task-scoped paths + evidence
```

## Role

```text
human or orchestrator
  └── coder-proto          implement + verify + final
        ├── discovery      map unknown areas
        └── reader         locate files for this task
```

## Quick start

```bash
./cortex-mk3 -m coder-proto --dry-run -p "noop"
./cortex-mk3 -m coder-proto -p "Add a null-check in <file> and verify with the narrowest make target"
```

As sub-agent:

```yaml
import:
  agents:
    - coder-proto   # or path to this tree
```

## Models (live agent.yml — change there, not here)

| Agent | Tier | Notes |
|-------|------|--------|
| coder-proto | primary + deepseek fallback | implement brain |
| discovery / reader | flash-free default | cheap evidence |

## Tools (root)

exec · grep · list · fs_read · **fs_write** · json · ask_tool · web_fetch  
Feed: working_directory  

Children: no `fs_write`. Discovery may use RO `exec` only.

## Caps (live)

| Agent | max_iter | history |
|-------|----------|---------|
| coder-proto | 20 | 80 |
| discovery | 20 | 60 |
| reader | 16 | 48 |

## Not in this prototype

tester · reviewer · log-triage · other Must/High roster — later slices.
