# coder-worker

Implementation module (writes + prove). Parent daily-driver is **`coder/`** (design/architecture).

```text
coder-worker/
├── agent.yml · system.md · persona.md
├── tools/      git_status · git_diff · project_test · build_detect
├── feeds/      repo_pulse
├── workflows/  implement · fix-failure · map-area · review-diff
├── skills/     evidence-first · smallest-diff · verify-before-final · match-local-style
├── prompts/    final-report · plan-phase · specialist-brief
└── agents/     discovery · reader · tester · reviewer
```

## Launch

```bash
# Prefer parent (design + delegate):
./cortex-mk3 -m manifests/agents/coder/agent.yml --tui experimental

# Direct worker (hands-on implement only):
./cortex-mk3 -m manifests/agents/coder-worker/agent.yml --tui experimental
```

## Roles

| Node | Writes? | Job |
|------|---------|-----|
| **coder** (parent) | no (default) | intent · architecture · taste · gate |
| **coder-worker** | yes | plan · implement · prove · report |
| discovery / reader | no | scout |
| tester / reviewer | no | verify / risk |
