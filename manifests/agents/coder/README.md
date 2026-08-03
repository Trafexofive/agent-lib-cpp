# coder (PROD)

Daily-driver implementation module. **No empty product surfaces** — if it is
listed in `agent.yml`, it is loaded at runtime.

```text
coder/
├── agent.yml · system.md · persona.md
├── tools/      git_status · git_diff · project_test · build_detect   (bash, real)
├── feeds/      repo_pulse                                            (bash, real)
├── workflows/  implement · fix-failure · map-area · review-diff
├── skills/     evidence-first · smallest-diff · verify-before-final · match-local-style
│                 → import.skills → live <skills> in prompt
├── prompts/    final-report · plan-phase · specialist-brief
│                 → import.files → live <prompt_modules>
└── agents/     discovery · reader · tester · reviewer
                (own agent.yml + system + persona; slim harness/small.md)
```

## Runtime inject

| import key | Prompt block |
|------------|--------------|
| `tools` / schemas | `<tools>` cards |
| `skills` | `<skills>` |
| `files` | `<prompt_modules>` |
| `workflows` | `<workflows>` spines |
| `agents` | `<sub_agents>` cards |

Missing import path → stderr warning, not silent theater.

## Roles

| Node | Writes? | Job |
|------|---------|-----|
| **coder** | yes | plan · implement · prove · report |
| discovery | no | area map |
| reader | no | task evidence |
| tester | no | narrow verify |
| reviewer | no | diff risk |

## Quick start

```bash
./cortex-mk3 -m manifests/agents/coder/agent.yml --dry-run -p "noop"
./cortex-mk3 -m manifests/agents/coder/agent.yml -p "Fix X and verify with the narrowest make target"
```

## Lab

`manifests/agents/coder-proto/` is experimental if present. This tree is PROD.
