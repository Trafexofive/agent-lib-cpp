# coder (PROD)

Daily-driver implementation module for Cortex-Prime MK3.

```text
coder/
├── agent.yml · system.md · persona.md
├── tools/     git_status · git_diff · project_test · build_detect
├── feeds/     repo_pulse (+ built-in working_directory)
├── workflows/ implement · fix-failure · map-area · review-diff
├── skills/    evidence-first · smallest-diff · verify-before-final · match-local-style
├── prompts/   final-report · plan-phase · specialist-brief
└── agents/    discovery · reader · tester · reviewer  (full units)
```

## Quick start

```bash
./cortex-mk3 -m coder --dry-run -p "noop"
./cortex-mk3 -m coder -p "Fix X in src/... and verify with the narrowest make target"
```

## Roles

| Node | Writes? | Job |
|------|---------|-----|
| **coder** | yes | plan · implement · prove · report |
| discovery | no | area map |
| reader | no | task evidence |
| tester | no | narrow verify |
| reviewer | no | diff risk |

## Models

See `agent.yml` `cognitive_engine` (live source of truth).

## Lab

`manifests/agents/coder-proto/` remains experimental. This tree is PROD.
