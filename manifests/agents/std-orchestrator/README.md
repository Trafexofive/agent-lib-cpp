# std-orchestrator (PROD)

Standard multi-agent control plane for the MK3 stdlib.

## Layout

```
std-orchestrator/
├── agent.yml
├── system.md
├── persona.md
├── README.md
├── workflows/route.yml
└── agents/
    ├── planner/      # decompose (no tools)
    ├── researcher/   # read-only recon
    └── skeptic/      # challenge plans/claims
```

Imports stdlib `coder` by bare name for implementation.

## Launch

```bash
./cortex-mk3 -m std-orchestrator --tui experimental
./cortex-mk3 -m manifests/agents/std-orchestrator -p "Audit X and propose a safe fix plan"
```

## Crew

| Agent | Role | Tools |
|-------|------|-------|
| std-orchestrator | route / synthesize | ask_tool |
| planner | ordered packages | none |
| researcher | read-only facts | list/grep/fs_read/context_peek |
| skeptic | challenge | light read |
| coder | implement+verify | full coding surface |

## vs orchestra (config/)

`config/agents/orchestra` is the experimental personality-variant playground.  
`manifests/agents/std-orchestrator` is the **shipped** stdlib control plane.
