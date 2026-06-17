# Standard Manifest Catalog

**Cortex-Prime MK3 Standard Library v3.1**

Last updated: 2026-06-17 — sovereign C++ classes, prompts/skills consolidation, workflow fix

## Architecture

```
manifests/                    ← Global scope — auto-loaded, name-resolvable
  built-in/tools/             6 tools (exec, list, grep, context_pin/peek/unpin)
  built-in/feeds/             3 feeds (system_clock, system_stats, working_directory)
  built-in/relics/            2 relics (session_journal, state_checkpoint)
  tools/                      5 tools (fs_read, fs_write, simple_fs_write, json, web_fetch)
  relics/                     5 managed relics (artifact_store, secret_store, event_bus, process_manager, file_watcher)
  agents/                     2 agents (assistant, orchestrator)
  workflows/                  2 workflows (code-review, workflow_spec)
  prompts/                    14 reusable prompt modules
  skills/                     2 MK3-native skills (mk3-manifest, harness-tuner)
  README.md                   Manifest system overview
  CATALOG.md                  This file
```

## Built-in Tools (6)

| Name | Description |
|------|-------------|
| exec | Execute shell commands |
| list | List files and directories |
| grep | Search files with regex |
| context_pin | Pin file to persistent agent context |
| context_peek | Peek at file for N cycles, then auto-evict |
| context_unpin | Remove pinned file from context |

## Global Tools (5)

| Name | Runtime | Description |
|------|---------|-------------|
| fs_read | python3 | Read file contents with offset/limit |
| fs_write | python3 | Structured JSON write/create files with append support |
| simple_fs_write | native | Text-body convenience writer; attrs carry path/append, body carries content |
| json | python3 | Parse, query, validate, transform JSON |
| web_fetch | python3 | HTTP GET/POST with status and headers |

## Built-in Feeds (3)

| Name | Description |
|------|-------------|
| system_clock | Current time: ISO8601, human, unix, date, time |
| system_stats | Hostname, platform, arch, kernel, CPU, memory |
| working_directory | CWD path, git root/branch/dirty |

## Built-in Relics (2)

| Name | Description |
|------|-------------|
| session_journal | Runtime-local session and context persistence |
| state_checkpoint | Agent state serialization for crash recovery |

## Global Relics (5)

| Name | Mode | Description |
|------|------|-------------|
| artifact_store | managed | Persistent file/data storage with versioning |
| secret_store | managed | AES-256-GCM encrypted secret storage |
| event_bus | managed | Pub/sub messaging for agent communication |
| process_manager | managed | Track and manage long-running subprocesses |
| file_watcher | managed | File change monitoring with event emission |

## Agent Modules (2)

| Agent | Model | Provider | Tools | Special |
|-------|-------|----------|-------|---------|
| assistant | gpt-5.5 | openai-codex | 7 | Default agent |
| orchestrator | GLM-5.1-FP8 | zen | 7 + 4 sub-agents | Delegates via `<action type="agent">` |

> More agents available in `staged-manifests/` — promoted here after battle-testing.

## Workflows

| Name | Description |
|------|-------------|
| code-review | Multi-step review: count lines, grep TODOs, read source, write summary |
| workflow_spec | Manifest format specification (reference, not instantiable) |

## Prompts (14)

| Prompt | Role |
|--------|------|
| `builder.md` | Implement features from spec |
| `tester.md` | Contract-driven testing (merged batch-test, write-test) |
| `debugger.md` | Hypothesis-driven debugging |
| `researcher.md` | Codebase + web research (merged research.md) |
| `refactorer.md` | Smell-first structural refactoring |
| `reviewer.md` | Correctness-first review (merged review.md) |
| `planner.md` | Planning and decomposition |
| `audit.md` | Structured audit/inspection |
| `git-sweep.md` | Git hygiene |
| `health-scan.md` | Quick health check |
| `verify-chain.md` | Verification discipline |
| `fix-lint.md` | Lint fix automation |
| `error-fallback.md` | Error recovery patterns |

## Skills (2)

| Skill | Purpose |
|-------|---------|
| `mk3-manifest` | Cortex-Prime MK3 module/manifest conventions |
| `harness-tuner` | Harness prompt compliance, protocol enforcement |

## C++ Sovereign Classes

| Module | Class | File | Responsibility |
|--------|-------|------|---------------|
| Tool | `Tool` | `src/tools/tool.hpp` | Owns def + callback/script execution |
| Feed | `Feed` | `src/feeds/feed.hpp` | Owns poll fn + caching + formatting |
| Relic | `Relic` (abstract) | `src/relics/relic.hpp` | Base class for all relic types |
| Workflow | `Workflow` | `src/workflows/workflow.hpp` | Owns manifest + serialization |
| Tools | `ToolRegistry` | `src/tools/registry.hpp` | Stores `Tool` objects, backward compat |
| Feeds | `FeedEngine` | `src/feeds/feed_engine.hpp` | Orchestrates `Feed` objects |
| Relics | `RelicDispatcher` | `src/relics/builtin_relics.hpp` | Routes via `RelicPtr`, HTTP fallback |
| Workflows | `WorkflowEngine` | `src/workflows/workflow_engine.hpp` | Loads/executes `Workflow` objects |


