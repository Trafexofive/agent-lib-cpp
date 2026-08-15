# brainstormer (PROD)

Ideation coordinator for Cortex-Prime MK3 stdlib.

## Layout

```
brainstormer/
├── agent.yml
├── system.md
├── persona.md
├── README.md
└── agents/
    ├── discovery/   # landscape + evidence (read/web)
    └── critic/      # kill weak ideas
```

## Launch

```bash
./cortex-mk3 -m brainstormer --tui experimental
./cortex-mk3 -m manifests/agents/brainstormer -p "10 ways to ship X without bloating Y"
```

## Tools

`list` `grep` `fs_read` `context_peek` `web_fetch` `ask_tool` `json` + feed `working_directory`

## Sub-agents

| Name | Role |
|------|------|
| discovery | prior art, constraints, repo/web evidence |
| critic | failure modes, kill criteria, cheap falsifiers |

## Non-goals

Implementation (use `coder`). Pure orchestration without ideation (use `std-orchestrator`).
