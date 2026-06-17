# Standard Manifest Library

Production-grade manifests for Cortex-Prime MK3.

## Module Standard
Every module follows the same pattern:
```
<module>/
├── README.md      ← Purpose, usage, dependencies, edge cases
├── <manifest>.yml ← Schema, endpoints, input/output definitions
└── <script>       ← Script file (if runtime != builtin)
```

## Layout
| Path | Content | Status |
|------|---------|--------|
| `built-in/tools/` | C++ compiled tools (exec, list, grep, context_pin/peek/unpin, ask_tool) | ✅ stable |
| `built-in/feeds/` | C++ compiled feeds (system_clock, stats, working_dir) | ✅ stable |
| `built-in/relics/` | Filesystem relics (session_journal, state_checkpoint) | ✅ stable |
| `agents/` | Agent manifests (default) | ✅ stable |
| `workflows/` | Workflow definitions (code-review, workflow_spec) | 🟡 growing |
| `prompts/` | Reusable task/persona prompts for bespoke agents | 🟡 seeded |
| `skills/` | Reusable agent operating policies/skills | 🟡 seeded |
| `_session/` | Persisted dynamic tools (auto-created, survives restarts) | ✅ stable |
| Runtime | Used by | Manifest field |
|---------|---------|----------------|
| `builtin` | C++ tools, feeds, relics | Compiles into binary |
| `python3` | fs_read, fs_write, json, web_fetch | `runtime: python3` |
| `docker` | artifact_store, secret_store, etc. | `runtime: docker` |

## Rules
- **Only production-ready manifests** live here. Staging → `config/staging/`
- **`built-in/`** = compiled into binary, resolved by name. No source code.
- `prompts/` and `skills/` are stdlib building blocks for bespoke agents, not auto-loaded runtime manifests.
- Each module is self-contained — imports reference sibling modules by relative path.
- **CATALOG.md** is maintained manually.

> Script tools (`fs_read`, `fs_write`, `json`, etc.) and Docker relics moved to `poc/` — graduate when battle-tested.

## Import Mechanics
- Agents load tools/relics/feeds via `import:` in their manifest
- Built-in modules resolved by name at runtime (no path needed)
- Script modules referenced by relative path from the manifest
- Docker relics referenced by module name; dispatcher handles container lifecycle
