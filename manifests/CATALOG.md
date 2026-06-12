# Standard Manifest Catalog

**Cortex-Prime MK3 Standard Library v3.1**

Last updated: 2026-06-09 — after agent.cpp split, manifest module build, session persistence

## Architecture

```
manifests/                    ← Global scope — auto-loaded, name-resolvable
  built-in/tools/             exec, list, grep, context_pin, context_peek, context_unpin
  built-in/feeds/             system_clock, system_stats, working_directory
  built-in/relics/            session_journal, state_checkpoint
  tools/                      fs_read, fs_write, json, web_fetch (yml + source + config)
  relics/                     artifact_store, secret_store, event_bus, process_manager, file_watcher
  agents/                     assistant (default agent, globally available)
  workflows/                  workflow_spec (manifest format reference)

config/                       ← Working directory — not auto-loaded
  agents/                     Agent modules (entrypoints via --manifest)
  tools/                      WIP tools (not yet graduated)
  relics/                     WIP relics
  feeds/                      WIP feeds
  staging/                    POC/experimental

Flow: build in config/ → graduate to manifests/ when needed globally
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

## Global Tools (4)

| Name | Runtime | Description |
|------|---------|-------------|
| fs_read | python3 | Read file contents with offset/limit |
| fs_write | python3 | Write/create files with append support |
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

## Agent Modules (8)

| Agent | Model | Provider | Tools | Special |
|-------|-------|----------|-------|---------|
| assistant | gpt-5.5 | openai-codex | 7 | Default agent, also in manifests/ |
| builder | deepseek-coder | openrouter-free | 8 | build_run tool, build_cache relic |
| reviewer | minimax-3 | openrouter-free | 8 | diff_analyze tool |
| researcher | minimax-3 | openrouter-free | 9 | web_search, html_extract tools |
| orchestrator | GLM-5.1-FP8 | zen | 7 + 4 sub-agents | Delegates via `<action type="agent">` |
| tester | GLM-5.1-FP8 | zen | 8 | test_run tool |
| architect | GLM-5.1-FP8 | zen | 8 | scaffold tool |
| artificer | GLM-5.1-FP8 | zen | 7 | Tool creation specialist |

## Workflows

| Name | Description |
|------|-------------|
| workflow_spec | Manifest format specification (reference, not instantiable) |

## Module Config

Every manifest module has a `config.yml` in its root for runtime behavior tuning:
- `runtime.timeout` — default timeout in seconds
- `runtime.max_retries` — retry count on failure
- `limits.max_output_size` — truncation threshold
- Module-specific keys (relic port, tool cache settings, agent temperature, etc.)

---

*Maintained manually. Last updated: session 9.*
