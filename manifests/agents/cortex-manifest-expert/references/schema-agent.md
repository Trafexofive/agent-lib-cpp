# Agent Manifest Schema

**Authored at:** `manifests/agents/<name>/agent.yml` (stdlib) or `config/agents/<name>/agent.yml` (user/local)

**Parsed by:** `src/core/manifest_loader.hpp::loadAgentConfig`

## Required fields

```yaml
kind: Agent
name: <snake_case>
version: "<semver>"
summary: "<one-line>"
```

## Cognitive engine

```yaml
cognitive_engine:
  primary:
    provider: deepseek            # deepseek, openrouter, openai-codex, groq, zen, together, fireworks
    model: deepseek-v4-pro
    parameters:
      temperature: 0.7
      max_tokens: 65536
  fallback:                       # optional, used when primary fails/empty
    provider: openrouter
    model: nex-2
```

The `parameters` block is passed to the provider. Recognized keys depend on the provider; common ones are `temperature`, `max_tokens`, `top_p`, `frequency_penalty`, `presence_penalty`. Unknown keys are silently ignored.

## Context (prompts)

```yaml
context:
  harness: ../../harness/default.md
  system: ../../system/default.md
  persona: ../../persona/default.md
```

Each is a path. **Context paths are overrides only** — if you omit one, the global default is used. Use overrides when you want this agent to differ from the project default.

Relative paths resolve against the manifest's directory. The harness system path may use `..` to escape into `manifests/`.

## Prompt building

```yaml
prompt_building:
  runtime_capabilities:
    return_schemas: false         # controls <returns> block emission
    usage_examples: false         # controls <examples> block emission
```

These flags control what the prompt assembler emits. Both default to `false` (leaner prompts).

## Imports

```yaml
import:
  tools:                          # list of tool names to expose
    - exec
    - grep
    - list
    - fs_read
    - fs_write
    - json
    - web_fetch
    - sleep
    - context_pin
    - context_peek
    - context_unpin
    - ask_tool
    - artifact
  feeds:                          # list of feed manifest paths (relative to this manifest)
    - ./morpheus_dashboard/feed.yml
    - ../shared/some_feed/feed.yml
  env: []                         # environment overrides
  agents: [default]               # sub-agents to expose
```

**Tool resolution** — names are matched against the tool registry (auto-discovered from `manifests/built-in/tools/`). C++-registered builtins are looked up first.

**Feed resolution** — names are interpreted as paths if they end in `.yml`, start with `/`, or start with `./`/`..`. Otherwise they're treated as builtin feed names (e.g., `system_clock`). This is the `isPathImport` check.

**Sub-agent resolution** — names are resolved:
1. First as a local name (e.g., `default` → `manifests/agents/default/`)
2. Then as `config/agents/<name>/`
3. Then as `manifests/agents/<name>/`

## Examples

- `manifests/agents/default/agent.yml` — minimal primary agent
- `config/agents/morpheus/agent.yml` — Morpheus, with full tool set + dashboard feed
- `playground/diagram-junky/manifests/agents/diagram-junky/agent.yml` — agent-local config

## Common mistakes

1. **Tool name with a typo** — the registry silently drops unknown tools. The model won't see them in `<action_available>`.
2. **Feed path without `./` prefix** — `feeds: [morpheus_dashboard/feed.yml]` is treated as a builtin name, not a path. Use `feeds: [./morpheus_dashboard/feed.yml]`.
3. **Omitting `kind: Agent`** — the manifest classifier falls back to defaults; you may not get what you expected.
4. **Absolute tool name in `tools:`** — there's no path resolution for tools, only names. Tool paths are baked into the registry at build time.
5. **`context.harness:` set to a file that doesn't exist** — the agent loads with an empty harness prompt, and the model has no protocol guidance. It will produce bare text that gets stripped.