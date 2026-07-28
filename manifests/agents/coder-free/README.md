# coder

> Specialized coding coordinator for Cortex-Prime MK3.
> **Brain:** `openai-codex/gpt-5.5` · **Specialists:** `deepseek/deepseek-v4-flash`
> Inspect → implement → verify. Smallest correct diff.

## Role in the stack

```
orchestrator (e.g. grok-4.5)
  └── coder (gpt-5.5)              ← this module
        ├── reader   (flash)       scout / evidence
        ├── tester   (flash)       focused verification
        └── reviewer (flash)       diff/risk pass
```

Coder is meant to be imported by a parent orchestrator. It owns implementation.
Specialists are intentionally small-model only so the expensive brain stays on hard edits.

## What it does

- Coordinates coding tasks with a gpt-5.5 primary loop
- Delegates bulk recon, verification campaigns, and risk review to flash specialists
- Applies production diffs itself (`fs_write` stays on coder)
- Returns evidence-backed summaries for the parent orchestrator

## Quick start

### As primary agent

```bash
./cortex-mk3 -m coder --dry-run -p "noop"
./cortex-mk3 -m coder -p "Add a null-check guard in src/core/dispatch.hpp and verify"
```

### As sub-agent of an orchestrator

```yaml
# parent agent.yml
import:
  agents:
    - coder
```

```xml
<action type="agent" name="coder" id="c1" mode="sync">
  Implement X in src/..., match local style, verify with the narrowest make target.
</action>
```

## Model policy

| Agent | Provider | Model | Why |
|-------|----------|-------|-----|
| **coder** (root) | `openai-codex` | `gpt-5.5` | frontier implementation brain |
| coder fallback | `deepseek` | `deepseek-v4-pro` | paid-capable backup if codex unavailable |
| **reader** | `deepseek` | `deepseek-v4-flash` | cheap recon |
| **tester** | `deepseek` | `deepseek-v4-flash` | cheap verify |
| **reviewer** | `deepseek` | `deepseek-v4-flash` | cheap risk pass |

Root runtime: 16 iterations, history cap 80, temp 0.25.
Specialists: 6–8 iterations, history 40–50, temp 0.1.

## Tool surface (root)

| Tool | Role |
|------|------|
| `list`, `grep`, `fs_read` | tight local inspect |
| `fs_write` | mutate (root only) |
| `exec` | build/test/git |
| `context_*` | sticky interfaces |
| `json`, `web_fetch`, `ask_tool` | structured / docs / gates |

Feed: `working_directory`.

## Specialists

| Name | Path | Tools | Forbidden |
|------|------|-------|-----------|
| `reader` | `agents/reader/` | list, grep, fs_read, context_peek | writes, exec |
| `tester` | `agents/tester/` | list, grep, fs_read, exec, json | feature edits |
| `reviewer` | `agents/reviewer/` | list, grep, fs_read, exec, context_peek | applying patches |

Resolved via `import.agents:` → `./agents/<name>/agent.yml`.

## Module layout

```
manifests/agents/coder/
├── README.md
├── agent.yml
├── system.md
├── persona.md
└── agents/
    ├── reader/{agent.yml,system.md,persona.md,README.md}
    ├── tester/{agent.yml,system.md,persona.md,README.md}
    └── reviewer/{agent.yml,system.md,persona.md,README.md}
```

## Relationship to staging coder

`staged-manifests/staging/agents/coder/` is a multi-agent POC with local Python tools.
This module is the production path: built-in tools only, gpt-5.5 root, flash specialists.

## Verification

```bash
./cortex-mk3 -m coder --dry-run -p "validate coder agent"
./cortex-mk3 list --agents
```

Expect root `openai-codex/gpt-5.5` and nested reader/tester/reviewer on `deepseek-v4-flash`.

## Limitations

- Specialists do not implement. Root owns writes.
- No custom patch tool — `fs_write` + `exec` only.
- Live quality depends on provider auth (`openai-codex` + `deepseek` keys).
- `--dry-run` validates load/prompt assembly only.
