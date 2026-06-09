# Standard Manifest Library

Production-grade manifests for Cortex-Prime MK3.

## Module Standard
Every module follows the same pattern:
```
<module>/
├── README.md      ← Purpose, usage, dependencies, edge cases
├── <manifest>.yml ← Schema, endpoints, input/output definitions
├── <script>       ← Script file (if runtime != builtin)
└── config.yml     ← Per-module runtime config
```

## Layout
| Path | Content | Status |
|------|---------|--------|
| `built-in/tools/` | C++ compiled + meta tools (14 total) | ✅ stable |
| `built-in/feeds/` | C++ compiled feeds (system_clock, stats, working_dir) | ✅ stable |
| `built-in/relics/` | Filesystem relics (session_journal, state_checkpoint) | ✅ stable |
| `tools/` | Script tools (bash, python3, node) | 🟡 growing |
| `feeds/` | Declarative feeds (build-status, git-activity, system-load) | 🟡 POC |
| `relics/` | Docker relics (artifact_store, secret_store, ...) | 🟡 dispatcher wired |
| `agents/` | Agent manifests (assistant, cpp-analyzer, bootstrapper) | ✅ stable |
| `workflows/` | Workflow definitions (full-audit, pr-review, code-review) | 🟡 growing |
| `_session/` | Persisted dynamic tools (auto-created, survives restarts) | ✅ stable |
| `modules/` | Reusable manifest modules (cpp-refactor-suite) | ✅ stable |
| Runtime | Used by | Manifest field |
|---------|---------|----------------|
| `builtin` | C++ tools, feeds, relics | Compiles into binary |
| `python3` | fs_read, fs_write, json, web_fetch | `runtime: python3` |
| `docker` | artifact_store, secret_store, etc. | `runtime: docker` |

## Rules
- **Only production-ready manifests** live here. Staging → `config/staging/`
- **`built-in/`** = compiled into binary, resolved by name. No source code.
- **Root-level** (tools/, relics/) = script modules with source alongside manifest.
- Each module is self-contained — imports reference sibling modules by relative path.
- **CATALOG.md** is maintained manually.

## Import Mechanics
- Agents load tools/relics/feeds via `import:` in their manifest
- Built-in modules resolved by name at runtime (no path needed)
- Script modules referenced by relative path from the manifest
- Docker relics referenced by module name; dispatcher handles container lifecycle
