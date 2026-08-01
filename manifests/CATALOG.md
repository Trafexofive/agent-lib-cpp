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
| **coder** | `agents/coder/` | **PROD daily-driver implementation module** |
| coder-proto | `agents/coder-proto/` | Lab / experimental coding roster |
| default | `agents/default/` | General agent |
| brainstormer | `agents/brainstormer/` | Ideation (+ discovery, critic) |
| std-orchestrator | `agents/std-orchestrator/` | Control plane (+ planner, researcher, skeptic) |
| agent-expert | `agents/agent-expert/` | Manifest authoring (experimental) |

## coder module (first full product unit)

```text
agents/coder/
  tools/     git_status, git_diff, project_test, build_detect
  feeds/     repo_pulse
  workflows/ implement, fix-failure, map-area, review-diff
  skills/    evidence-first, smallest-diff, verify-before-final, match-local-style
  agents/    discovery, reader, tester, reviewer
```

## Nested specialists

- **coder** → discovery, reader, tester, reviewer  
- **coder-proto** → discovery, reader  
- **brainstormer** → discovery, critic  
- **std-orchestrator** → planner, researcher, skeptic  

## Built-ins

| Kind | Location |
|------|----------|
| tools | `built-in/tools/*` |
| feeds | `built-in/feeds/*` |
| harness | `harness/default.md` (+ small/medium/big) |
| prompts | `prompts/*` (global modules) |
| skills | `skills/*` |
| workflows | `workflows/*` |

## Hub

Dashboard → **Manifests** (`a`): recursive registry.
