---
name: mk3-manifest
description: >
  Create self-contained, portable manifest modules for Cortex-Prime MK3 agents. Use when the user
  asks to create an MK3 agent manifest, tool manifest, feed manifest, relic manifest, or workflow
  manifest. Also trigger for requests like "write an agent.yml for X", "scaffold an MK3 tool",
  "create a feed for Y", "set up a docker relic", "design an MK3 workflow", or "build a portable
  agent module for Cortex-Prime MK3". Covers the full manifest ecosystem: agents, tools, feeds,
  relics, workflows, sandbox modes, and import resolution.
---

# MK3 Manifest Masterclass

Create self-contained, portable manifest modules for Cortex-Prime MK3 agents.

## Module Portability

A module works from **any path on the filesystem**. All internal references are relative to the module root (where `agent.yml` lives). The `Cortex-Prime-MK1/services/agent-lib-MK3/` layout is organizational convention, not a requirement.

```
<module>/                           ← This is the root. Can be anywhere.
    agent.yml                       ← All paths resolve relative to HERE.
    ./system-prompts/agent.md       ← agent.yml references this as "./system-prompts/agent.md"
    ./tools/scaffold/tool.yml       ← referenced as "./tools/scaffold/tool.yml"
```

## Full Module Tree

```
<module>/
├── README.md                       ← MANDATORY. Purpose, usage, deps, edge cases.
├── agent.yml                       ← Kind: Agent, cognitive_engine, persona, import
├── system-prompts/                 ← Persona markdown files
│   └── agent.md                    ← System prompt: persona, rules, examples
├── context/                        ← Files for sandbox mount or context-pinning
│   └── ...                         ← Freeform structure, domain-dependent
├── tools/                          ← Agent-local tools
│   └── <tool>/                     ← Tool module
│       ├── README.md               ← Tool purpose, contract, examples
│       ├── tool.yml                ← Schema: input, output, examples, runtime
│       └── src/                    ← Source code (NEVER at module root)
│           ├── <concern>/          ← Split by concern, not monolithic
│           │   └── <file>.py
│           └── main.py             ← Entrypoint
├── feeds/                          ← Agent-local feeds
│   └── <feed>/
│       ├── README.md
│       ├── feed.yml
│       └── src/
│           └── ...
├── workflows/                      ← Workflow manifests
│   └── <workflow>/
│       ├── README.md
│       └── <workflow>.yml
├── relics/                         ← Agent-local relics
│   └── <relic>/
│       ├── README.md
│       ├── relic.yml
│       └── src/
│           └── ...
├── agents/                         ← Sub-agents (full modules, infinite recursion)
│   └── <sub-agent>/
│       ├── README.md
│       ├── agent.yml
│       ├── system-prompts/
│       ├── tools/
│       ├── feeds/
│       └── agents/                 ← Can nest further
```

## Iron Rules

1. **No code at module root.** All source lives in `src/<concern>/`.
2. **Split by concern.** `src/analysis/extractor.py`, not `src/tool.py`.
3. **README on every component.** Module README, tool README, feed README.
4. **kebab-case dirs** (`my-agent`, `plan-progress`). **snake_case YAML keys** (`output_schema`).
5. **Relative paths.** Everything resolves relative to the YAML file containing the reference.
6. **Narrow imports.** An agent imports only what it actually uses. 3 tools max for specialists.

## Agent Manifest — Full Schema

Every YAML key and what it maps to at runtime:

```yaml
kind: Agent                         # MUST be "Agent" (ManifestYaml::find checks this)
name: my-agent                      # → AgentConfig.name
version: "1.0"                      # → AgentConfig.version
summary: "One sentence purpose."    # → AgentConfig.summary

cognitive_engine:                   # → AgentConfig fields
  primary:
    provider: deepseek              # → AgentConfig.provider (default: "deepseek")
    model: deepseek-chat            # → AgentConfig.model (default: "deepseek-chat")
    parameters:
      temperature: 0.7              # → AgentConfig.temperature (stod)
      max_tokens: 4096              # → AgentConfig.maxTokens (stoi)
  fallback:                         # → AgentConfig.fallbackProvider / fallbackModel
    provider: openrouter
    model: meta-llama/llama-3.1-8b-instruct

persona:                            # → AgentConfig.systemPromptPath
  agent: ./system-prompts/agent.md  # Resolved relative to agent.yml directory

runtime:                            # → AgentConfig (optional overrides)
  max_iterations: 4                 # → AgentConfig.iterationCap (default: 3)
  history_cap: 40                   # → AgentConfig.historyCap (default: 40)

sandbox:                            # → AgentConfig.sandboxMode / sandboxRuntime / sandboxImage
  mode: docker | process | chroot   # default: "process"
  image: "ubuntu:24.04"             # for docker mode

import:                             # Loaded by ManifestLoader at agent construction
  tools:                            # → ManifestLoader::loadTools() → agent.addTool()
    - exec                          #   Plain name = built-in, resolved by ToolRegistry
    - grep                          #   Built-in: exec, grep, list, json, fs_read, fs_write,
    - list                          #             web_fetch, context_pin, context_peek, context_unpin
    - ./tools/my-tool/tool.yml     #   Path = local tool manifest, loaded by loadToolSchema()
  feeds:                            # → FeedEngine::loadFeedManifest() (if path)
    - system-clock                  #   Built-in feeds: system-clock, system-stats, working-directory
    - ./feeds/my-feed/feed.yml     #   Path = local feed manifest
  agents:                           # → ManifestLoader::loadSubAgents()
    - reviewer                      #   Looks up ../reviewer/agent.yml relative to parent
  relics:                           # → DockerRelicDispatcher::loadRelic()
    - artifact_store                #   Loaded from manifests/relics/<name>/relic.yml
  workflows:                        # → ManifestLoader::loadWorkflows()
    - ./workflows/deploy.yml       #   Path = local workflow manifest
  files:                            # → ManifestLoader::loadFiles() → AgentConfig.sandboxFiles
    - domain-knowledge/             #   if sandbox = docker: COPY into container
    - schema.json                   #   if sandbox = process: context-pin the file
```

### Provider Notes

Available providers and their canonical models:

| Provider | CLI flag | Key models |
|----------|---------|------------|
| `deepseek` | `--provider deepseek` | deepseek-chat, deepseek-v4-pro |
| `openrouter` | `--provider openrouter` | meta-llama/llama-3.1-8b-instruct, minimax-3 |
| `groq` | `--provider groq` | llama-3.1-8b-instant, mixtral-8x7b |
| `zen` | `--provider zen` | zai-org/GLM-5.1-FP8 (free tier) |
| `openrouter-free` | `--provider openrouter-free` | Uses OpenRouter free-tier models |

## Tool Manifest — Full Schema

```yaml
kind: Tool                          # MUST be "Tool"
name: my_tool                       # Tool name (agent uses in <action name="...">)
version: "1.0"
summary: "What this tool does."

runtime: python3 | bash | node | builtin
entrypoint: ./src/main.py           # Relative to tool.yml. Executed as: <runtime> <entrypoint> [json_params]

input_schema:                       # JSON Schema. Injected into <tool> XML block in harness prompt.
  type: object
  required: [path]                  # Required params (array of strings)
  properties:
    path:
      type: string
      description: "File to process"
    mode:
      type: string
      enum: [fast, thorough]
      default: fast

output_schema:                      # JSON Schema for return value.
  type: object                      # Tool must return JSON matching this.
  properties:
    success:
      type: boolean
    result:
      type: string

examples:                           # Injected into <tool><examples> XML block.
  - description: "Basic usage"      # The model sees these as part of its prompt.
    params:
      path: "src/main.cpp"
      mode: fast

on_error: abort | retry | skip      # Default: abort (agent stops on failure)
timeout: 30                         # Seconds. Default: 30
max_retries: 0                      # For on_error: retry
```

### Tool Script Contract

The runtime calls: `<runtime> <entrypoint> '<json_params>'`

Example execution:
```bash
python3 ./tools/my-tool/src/main.py '{"path": "src/main.cpp", "mode": "fast"}'
```

**Contract:**
- Receives params as **first CLI argument** (JSON string)
- Outputs result as **JSON to stdout** (single line, minified)
- Exit code **0 = success**, **non-zero = error**
- Stderr is captured and logged but not returned to agent

**Minimal Python template:**
```python
#!/usr/bin/env python3
import sys, json

def main(params):
    # Your logic here
    return {"success": True, "result": "done"}

if __name__ == "__main__":
    params = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    result = main(params)
    print(json.dumps(result))
```

**Minimal Bash template:**
```bash
#!/usr/bin/env bash
PARAMS=$(echo "$1" | python3 -c "import sys,json; print(json.dumps(json.load(sys.stdin)))" 2>/dev/null || echo '{}')
# Your logic here
echo '{"success": true, "result": "done"}'
```

## Feed Manifest — Full Schema

```yaml
kind: Feed
name: my_feed                        # XML tag: <my_feed>...</my_feed>
version: "1.0"
summary: "What this feed reports."

runtime: python3 | bash | node | builtin
entrypoint: ./src/poller/poll.sh    # Relative to feed.yml
interval_secs: 60                   # Poll frequency. Default: 60

output_schema:
  type: object
  properties:
    status:
      type: string
    count:
      type: integer
```

### Feed Script Contract

The runtime calls: `<runtime> <entrypoint>` (no params). Called on every poll cycle.

**Contract:**
- Outputs **JSON to stdout** (single line)
- Must be **idempotent** and **fast** (<1 second)
- Exit code non-zero → feed marked as failed, previous value retained
- Re-executed every `interval_secs`

**Feed with tool-call support (upcoming):**
Feed scripts can call built-in tools via a tool dispatcher provided by the feed engine. The feed runtime exposes a `call_tool(name, params)` function so feed scripts can gather data dynamically using tools like `grep`, `list`, `web_fetch`.

## Relic Manifest — Full Schema

```yaml
kind: Relic
name: my_relic
version: "1.0"
summary: "What this relic provides."

mode: managed | remote | builtin    # managed = Docker, remote = HTTP API, builtin = filesystem
port: 8200                          # For managed/remote only
health_path: /health                # Health check endpoint

endpoints:                          # Available operations
  store:
    method: POST
    path: /store
    description: "Store data"
  retrieve:
    method: GET
    path: /retrieve
    description: "Retrieve data"
```

### Relic Dispatch Flow

1. Agent emits `<action type="relic" name="artifact_store" id="r1" mode="sync">{"endpoint":"store","key":"x"}</action>`
2. `dispatchRelic()` checks `DockerRelicDispatcher::getRelic("artifact_store")`
3. If **managed**: checks container health → if down, `docker-compose up -d` → waits for `/health` → HTTP call
4. If **remote**: direct HTTP call to the configured endpoint
5. If **builtin**: delegates to filesystem-based `RelicDispatcher`
6. Managed relics are **never shut down** — they stay running for subsequent calls

## Workflow Manifest

```yaml
kind: Workflow
name: deploy
version: "1.0"
summary: "Deployment pipeline"

steps:
  - id: build                        # Unique step ID
    type: tool                       # tool | agent | condition | loop | parallel | workflow
    tool: exec                       # Tool name (for type: tool)
    params:                          # Params passed to tool
      command: "make build"
    on_error: abort                  # abort | retry | skip
    max_retries: 0                   # For on_error: retry
    timeout: 30                      # Seconds

  - id: test
    type: tool
    tool: exec
    params:
      command: "make test"
    on_error: abort

  - id: condition_check              # Conditional branching
    type: condition
    condition: "build.success == true"
    then:                            # Steps if condition true
      - id: deploy
        type: agent
        agent: builder
    else:                            # Steps if false
      - id: notify
        type: tool
        tool: web_fetch
```

## Import Resolution Algorithm

When an agent manifest contains `import:`:

```
import:
  tools: [exec, ./tools/my-tool/tool.yml]
  agents: [reviewer]
  relics: [artifact_store]
```

Resolution:
1. **Plain name** (`exec`, `grep`) → `ToolRegistry::has(name)` → built-in, no file lookup
2. **Path** (`./tools/my-tool/tool.yml`) → resolved relative to `agent.yml` directory
3. **Sub-agent** (`reviewer`) → looks for `<parent>/../reviewer/agent.yml`
4. **Relic** (`artifact_store`) → `DockerRelicDispatcher::loadRelic("manifests/relics/artifact_store/")`
5. **Feed path** (`./feeds/my-feed/feed.yml`) → `FeedEngine::loadFeedManifest(path)`
6. **Workflow path** → `WorkflowEngine::load(path)` → `toXml()` injected into harness prompt

## How Manifests Wire Into the Runtime

User runs: `./cortex-mk3 -m <path>/agent.yml --model deepseek-chat --prompt "task"`

```
main.cpp
  ├── ManifestLoader::loadAgentConfig(manifestPath)
  │   └── Populates AgentConfig (name, provider, model, temperature, sandbox...)
  ├── ManifestLoader::loadConfigOverrides(manifestPath, cfg)
  │   └── Manifest carries all config natively (no config.yml)
  ├── providers::createProvider(cfg.provider, cfg.model)
  ├── Agent(cfg, provider)
  ├── ManifestLoader::loadTools(manifestPath, agent)
  │   ├── For each name in import.tools:
  │   │   ├── Path? → loadToolSchema(path) → agent.addTool({name, description})
  │   │   └── Plain? → loadBuiltinToolSchema(name) → schema injected
  │   └── schemas → toolSchemasToXml() → agent.setEnv("__TOOL_SCHEMAS__", xml)
  ├── ManifestLoader::loadSubAgents(manifestPath, agent, providerName)
  │   └── For each name in import.agents → load sub-agent → agent.addSubAgent()
  ├── ManifestLoader::loadWorkflows(manifestPath)
  │   └── WorkflowEngine::load() → toXml() → agent.setEnv("__WORKFLOW_XML__", xml)
  └── agent.prompt(input)
      └── buildSystemPrompt()
          ├── <harness> → protocol, info
          ├── <system> → persona, tools (including __TOOL_SCHEMAS__), relics,
          │              sub-agents, workflows (__WORKFLOW_XML__), context, cwd
          ├── <feeds> → FeedEngine::pollAll() → <feed_name>...</feed_name>
          ├── <context_feeds> → accumulated from prior iterations
          └── <history> → conversation turns
```

## Agent Protocol (What the Model Emits)

The model communicates through XML tags. These are parsed by the protocol parser in `parser.cpp`.

```xml
<!-- User-visible response -->
<response>Hello, here are the files:</response>

<!-- Final answer — conversation ends -->
<response final="true">Task complete.</response>

<!-- Tool call -->
<action type="tool" name="list" id="ls1" mode="sync">
  {"path": ".", "recursive": true}
</action>

<!-- Sub-agent delegation -->
<action type="agent" name="reviewer" id="r1" mode="sync">
  {"instruction": "Review the code in src/"}
</action>

<!-- Relic call -->
<action type="relic" name="artifact_store" id="a1" mode="sync">
  {"endpoint": "store", "key": "result", "value": "..."}
</action>

<!-- Context feed (model requests persistent context) -->
<context_feed source="model-notes">
  Important finding: the config uses port 8080.
</context_feed>
```

**Rules the model must follow:**
- Only `<response>` content reaches the user
- `<response final="true">` ends the conversation
- Action results appear as `<turn role="system"><result id="...">...</result></turn>` in history
- Tool params are JSON inside the action tag body
- Agent params use `{"instruction": "..."}`
- Relic params use `{"endpoint": "name", ...}`

## Sandbox Modes

| Mode | Flag | Behavior | context/ files |
|------|------|----------|---------------|
| `process` | default | Runs on host, same filesystem | Pinned to context via context_pin |
| `docker` | `agent.yml → sandbox.mode: docker` | Docker container, isolated | COPY'd into /workspace at build time |
| `chroot` | `sandbox.mode: chroot` | Chroot jail | Bind-mounted into chroot |

**Sandbox launch flow:**
```
./cortex-mk3 -m agent.yml
  → ManifestLoader::loadAgentConfig() reads sandbox.mode
  → if "docker" and not already in container (/.dockerenv):
      → sandbox::launchDocker() builds image, runs container
  → if "process":
      → loadFiles() returns paths, agent.setEnv("__SANDBOX_FILES__", ...)
```

## Test Cycle

```bash
# 1. Write the agent manifest
mkdir -p my-agent/{system-prompts,tools/my-tool/src/analysis}

# 2. Test with a live prompt
./cortex-mk3 -m my-agent/agent.yml --model deepseek-chat --prompt "test your task"

# 3. Check verbose output for prompt structure
./cortex-mk3 --verbose -m my-agent/agent.yml --model deepseek-chat --prompt "test" 2>&1 | head -80

# 4. Automated tests (if applicable)
make test-feeds        # Feed manifest tests
make test-relics       # Docker relic dispatcher tests
make test-protocol     # Protocol parser tests
```

## Anti-Patterns

| Don't | Do |
|-------|-----|
| `tools/scaffold.py` at root | `tools/scaffold/src/analysis/extractor.py` |
| One monolithic 500-line script | Multiple files split by concern |
| Import all 10 tools | Import only the 2-3 the agent actually needs |
| No README | README on every component |
| God agent (does everything) | Specialized agent (one job) |
| Flat files, no directories | Directory per module, `src/` per tool/feed |
| config.yml (deprecated) | All config lives in manifest.yml natively |
| Hardcoded absolute paths | Relative paths from manifest location |
