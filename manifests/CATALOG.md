# Standard Manifest Catalog (PROD)

Last updated: 2026-07-21

## Scope

| Tree | Role |
|------|------|
| `manifests/` | PROD / std — hub recursive scan |
| `config/` | DEV / MVP — not auto-hubbed |

## Top-level agents

| Name | Path | Role |
|------|------|------|
| default | `agents/default/` | General agent |
| coder | `agents/coder/` | Coding coordinator (+ reader/tester/reviewer) |
| brainstormer | `agents/brainstormer/` | Ideation (+ discovery/critic) |
| std-orchestrator | `agents/std-orchestrator/` | Control plane (+ planner/researcher/skeptic → coder) |

## Nested specialists (also listed in hub)

- coder → reader, tester, reviewer  
- brainstormer → discovery, critic  
- std-orchestrator → planner, researcher, skeptic  

## Hub

Dashboard → **Manifests** (`a`): recursive registry.  
Empty state prints searched roots. Discovery walks up from `agent.yml` and the binary path; skips empty placeholder `manifests/` dirs.
