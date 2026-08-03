# Standard Manifest Catalog (PROD)

Last updated: 2026-08-03

## Scope

| Tree | Role |
|------|------|
| `manifests/` | PROD / std — hub recursive scan |
| `config/` | DEV / MVP — not auto-hubbed |

## Top-level agents

| Name | Path | Role |
|------|------|------|
| **coder** | `agents/coder/` | **PROD daily-driver — design/architecture owner** |
| **coder-worker** | `agents/coder-worker/` | Implementation unit (writes); child of coder |
| default | `agents/default/` | General agent |
| brainstormer | `agents/brainstormer/` | Ideation |
| agent-expert | `agents/agent-expert/` | Manifest authoring (experimental) |

## Coding stack

```text
agents/coder/                 parent — taste, architecture, accept gate
  import.agents: coder-worker

agents/coder-worker/          implementer
  tools/     git_status, git_diff, project_test, build_detect
  feeds/     repo_pulse
  workflows/ implement, fix-failure, map-area, review-diff
  skills/    evidence-first, smallest-diff, verify-before-final, match-local-style
  agents/    discovery, reader, tester, reviewer
```

## Nested specialists

- **coder** → coder-worker  
- **coder-worker** → discovery, reader, tester, reviewer  
- **brainstormer** → (see its tree)  
- **agent-expert** → (see its tree)  

## Built-ins

| Kind | Location |
|------|----------|
| tools | `built-in/tools/*` |
| feeds | `built-in/feeds/*` |
| harness | `harness/default.md` (+ small/medium/big) |
| prompts | `prompts/*` (global modules) |
| skills | `skills/*` |
| workflows | `workflows/*` |
