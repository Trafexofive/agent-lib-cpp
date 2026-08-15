# coder (PROD parent)

Design/architecture **owner**. Daily-driver entrypoint.

Implementation lives in **`coder-worker/`** (writes + prove + scouts/gates).

```text
coder/                         ← you launch this
├── agent.yml · system.md · persona.md
├── skills/    architecture-first · taste-and-clarity · …
├── prompts/   brief-worker · accept-gate
└── import → coder-worker      ← sole implementer child

coder-worker/                  ← hands-on unit
├── tools · feeds · workflows · skills
└── agents/ discovery reader tester reviewer
```

## Launch

```bash
./cortex-mk3 -m manifests/agents/coder/agent.yml --tui experimental
# or bare name if manifests root resolves:
./cortex-mk3 -m coder --tui experimental
```

## Split of power

| Agent | Writes? | Job |
|-------|---------|-----|
| **coder** | no | intent, architecture, taste, accept/reject |
| **coder-worker** | yes | implement + verify |
| worker specialists | no | scout / test / review |

## Note

Former single-module `coder/` was renamed to `coder-worker/`. This tree is the new parent.
