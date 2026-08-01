# Standard Manifest Catalog (PROD)

Last updated: 2026-08-01

## Scope

| Tree | Role |
|------|------|
| `manifests/` | PROD / std — hub recursive scan |
| `config/` | DEV / MVP — not auto-hubbed |

## Top-level agents

| Name | Path | Role |
|------|------|------|
| default | `agents/default/` | General agent |
| coder-proto | `agents/coder-proto/` | Coding coordinator (+ discovery, reader) |
| brainstormer | `agents/brainstormer/` | Ideation (+ discovery, critic) |
| std-orchestrator | `agents/std-orchestrator/` | Control plane (+ planner, researcher, skeptic) |
| agent-expert | `agents/agent-expert/` | Manifest/agent authoring (experimental) |

## Nested specialists (hub-listed)

- **coder-proto** → discovery, reader  
- **brainstormer** → discovery, critic  
- **std-orchestrator** → planner, researcher, skeptic  
- **agent-expert** → planner, temperature-bench, …  

## Built-ins

| Kind | Location |
|------|----------|
| tools | `built-in/tools/*` (exec, list, grep, fs_*, json, web_fetch, sleep, artifact, ask_tool, context_*) |
| feeds | `built-in/feeds/*` (system_clock, working_directory, system_stats) |
| harness | `harness/default.md` (+ small/medium/big) |
| prompts | `prompts/*` (modules — not agents) |
| skills | `skills/*` |
| workflows | `workflows/*` |

## Hub

Dashboard → **Manifests** (`a`): recursive registry.  
Discovery walks up from CWD and binary path; skips empty placeholder dirs.

## Skills

- `skills/inkcell` — inkcell TUI framework  
- `skills/mk3-manifest` — MK3 manifest authoring  
- `skills/harness-tuner` — harness compliance tuning  
