# Cortex-Prime MK3 — Agent Runtime

Production-grade AI agent runtime with native DeepSeek inference, sandboxed Docker execution,
streaming XML protocol, and a standard library of tools, feeds, and relics.

## Quickstart

```bash
make cortex-mk3
./cortex-mk3 --model deepseek-chat                    # interactive
./cortex-mk3 --prompt "list files"                    # one-shot
./cortex-mk3 --raw --prompt "list files"              # raw XML output
./cortex-mk3 --verbose --prompt "what time is it"     # full debug
./tests/sandbox/run.sh assistant                       # Docker sandbox test
```

## Architecture

```
src/
  core/          Agent loop, prompt builder, config types
  protocol/      Streaming XML parser (<action>, <response>, <context_feed>)
  providers/     LLM backends (DeepSeek, OpenRouter, Groq, Zen, Together, Fireworks)
  tools/         Built-in tool implementations (exec, grep, fs_read, ...)
  feeds/         System feed pollers (clock, stats, working dir)
  relics/        Built-in relics (session_journal, state_checkpoint)
  workflows/     Workflow engine (step executor)
  session/       Session persistence
  testing/       Protocol test runner, parser tests

manifests/
  built-in/      Tools, feeds, relics compiled into binary
  tools/         Script tools (fs_read, fs_write, json, web_fetch)
  relics/        Docker relics (artifact_store, secret_store, ...)
  agents/        Production agent manifests
  workflows/     Workflow spec

config/
  agents/        Agent manifests under development
  staging/       POC experiments — not loaded by default
```

## Protocol

Agents communicate via streaming XML tags:

```xml
<action type="tool" name="grep" id="g1" mode="sync">{"pattern":"TODO","path":"src/"}</action>
<response final="true">Found 3 TODOs in src/agent.cpp</response>
```

- `<action>` — call a tool, agent, or relic
- `<response>` — user-visible output (only this reaches the user)
- `<response final="true">` — final response, conversation ends
- Untagged text is internal harness chatter — never shown to user

## Standard Library

### Tools (14)
| Tool | Type | Description |
|------|------|-------------|
| exec | built-in | Execute shell commands (timeout-enforced) |
| list | built-in | List directory contents |
| grep | built-in | Search files with regex |
| fs_read | built-in | Read file with offset/limit |
| fs_write | built-in | Write/create files |
| context_pin | built-in | Pin file to persistent context |
| context_peek | built-in | Read file for N cycles, auto-evict |
| context_unpin | built-in | Remove pinned file from context |
| json | built-in | Parse, query, validate JSON |
| web_fetch | built-in | HTTP GET/POST |
| ask_tool | built-in | Structured user interaction via cards |
| reload_manifests | meta | Hot-reload tool manifests from disk |
| disable_builtin | meta | Disable a built-in tool by name |
| enable_builtin | meta | Re-enable a disabled built-in |

### Feeds (3) — auto-injected into prompt
| Feed | Data |
|------|------|
| system_clock | ISO8601, unix, date, time |
| system_stats | Hostname, platform, CPU, memory, PID |
| working_directory | CWD, git repo, branch, dirty, commit |

### Built-in Relics (2)
| Relic | Description |
|-------|-------------|
| session_journal | Session event recording and querying (filesystem) |
| state_checkpoint | Agent state snapshots for crash recovery (filesystem) |

### Docker Relics (5) — containers exist, dispatcher WIP
| Relic | Description |
|-------|-------------|
| artifact_store | Persistent file/data storage with versioning |
| secret_store | AES-256-GCM encrypted secret storage |
| event_bus | Pub/sub messaging |
| process_manager | Long-running subprocess tracking |
| file_watcher | File change monitoring |

## CLI

```
Usage: cortex-mk3 [options]

  -p, --prompt <text>     One-shot prompt
  -m, --manifest <path>   Agent manifest YAML
  --provider <name>       deepseek, openrouter, groq, zen, together, fireworks
  --model <name>          Model name (default: deepseek-v4-pro)
  --system-prompt <path>  Override system prompt
  --harness-prompt <path> Tuning harness prompt
  --raw                   Raw XML output (no sanitization)
  --verbose, -V           Dump full prompts each iteration
  --debug                 Iteration-level stats
  --session <id>          Session persistence
  --ephemeral             Don't save session

Interactive commands:
  /help, /tools, /sessions, /load, /save, /clear, /providers, /model, /quit
```

## Testing

```bash
make test-parser        # 9/9 parser tests
make test-protocol      # 23/25 protocol tests (2 known expected failures)
make tune-prompt SUITE=assistant   # prompt tuning harness
./tests/sandbox/run.sh assistant   # Docker sandbox test
```

## MK3 vs MK2

- Compiled C++ (was Python) — sub-ms parser, zero Python overhead
- Streaming parser — tokens processed as they arrive
- Feed engine — system context auto-injected
- Built-in relics — no Docker required for core persistence
- Docker sandbox — multi-stage build, non-root user, read-only mounts
- Production model: deepseek-chat (was minimax-3/openrouter-free)
