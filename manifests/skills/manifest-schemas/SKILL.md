---
name: manifest-schemas
description: >
  Cortex-Prime MK3 / agent-lib-cpp manifest schema encyclopedia. Activate when
  you need exact YAML keys, kind shapes, import resolution rules, module tree
  layout, builtin catalogs, or field→runtime maps for Agent, Tool, Feed, Relic,
  or Workflow manifests. Triggers: "manifest schema", "agent.yml keys",
  "tool.yml schema", "what does import.X do", "builtin tools list", "module
  tree layout", "feed.yml fields", "relic endpoints schema", "workflow steps
  schema", "manifest-schemas". Companion to manifest-expert (judgment/PE/package
  design). Prefer manifest-expert for architecture and shipping; load this skill
  for exhaustive key-level reference. Formerly mk3-manifest.
---

# Manifest Schemas

**Schema encyclopedia only.** Decisions, PE stack, sandbox/compaction judgment →
`manifest-expert`. Protocol law → `docs/protocol/CANON.md`. Compliance grind →
`harness-tuner`.

**Repo:** `/home/mlamkadm/repos/active/agent-lib-cpp`  
**Docs index:** `docs/manifests/`  
**Loader:** `src/core/manifest_loader.hpp` · **types:** `src/core/types.hpp`

---

## Locked rules

1. **Sub-agents are isolated.** A child’s `import:` is exactly its own set — no parent inheritance of tools/feeds/relics.
2. **Built-ins are opt-in.** Listing nothing means no `exec`/`fs_read`/…. Catalog ≠ auto-load.
3. **No `config:` block.** Use `import.env`, tool params, or code.
4. **`extends:` deferred.** Copy-paste bases for now.
5. **`import.files` ≠ sandbox mounts.** Files → prompt `<module>`; live FS → `sandbox.bind` / `sandbox.files`.
6. **Paths resolve relative to the YAML file that holds the reference** (module root = dir of that YAML).

Canonical surface docs:

| Topic | Doc |
|---|---|
| Sandbox | `docs/manifests/sandbox.md` |
| Compaction | `docs/manifests/compaction.md` |
| Schemas narrative | `docs/manifests/MANIFEST_SCHEMAS.md` |
| Quick ref | `docs/manifests/MANIFEST_QUICK_REFERENCE.md` |
| Agent sketch | `docs/manifests/agent.schema.yml` |

---

## Kinds

| `kind:` | File | Role |
|---|---|---|
| `Agent` | `agent.yml` | LLM entity + imports + runtime |
| `Tool` | `tool.yml` | Executable capability |
| `Feed` | `feed.yml` | Periodic/on-demand context |
| `Relic` | `relic.yml` | Managed/remote service |
| `Workflow` | `workflow.yml` / `workflows/<name>.yml` | Multi-step chain |

Common header (all kinds):

```yaml
kind: Agent                    # required — exact spelling
name: my-agent                 # required
version: "1.0"                 # required
summary: "One-line description"
# description: >
#   Longer text.
# tags: [example]
```

> Kind-check is strictest for `Agent` today; other kinds parse by convention.

---

## Portable module tree

```
<module>/                          ← root = directory of agent.yml
├── README.md
├── agent.yml
├── system-prompts/                # persona / system PE files
│   ├── persona.md
│   └── system.md
├── prompts/                       # import.files modules (static)
├── context/                       # optional seed material for binds
├── tools/<tool>/
│   ├── README.md
│   ├── tool.yml
│   └── src/<concern>/…            # NEVER code at tool root
├── feeds/<feed>/
│   ├── README.md
│   ├── feed.yml
│   └── src/…
├── workflows/<workflow>.yml
├── relics/<relic>/
│   ├── README.md
│   ├── relic.yml
│   └── …
└── agents/<child>/                # full nested modules
    └── agent.yml
```

**Conventions:** kebab-case dirs · snake_case YAML keys · relative paths only · README per shippable component.

---

## Agent (`kind: Agent`)

```yaml
kind: Agent
name: my-agent
version: "1.0"
summary: "…"

cognitive_engine:
  primary:
    provider: deepseek              # host must know this provider id
    model: deepseek-chat
    parameters:                     # optional subset
      temperature: 0.3
      max_tokens: 8192
      # top_p, top_k, presence_penalty, frequency_penalty
  fallback:                         # optional
    provider: openrouter
    model: meta-llama/llama-3.1-8b-instruct
  # thinking: true                  # if supported by stack

# --- Prompt paths (prefer context: split for non-trivial agents) ---
context:
  harness: ../../../manifests/harness/default.md
  system: ./system-prompts/system.md
  persona: ./system-prompts/persona.md
# Legacy combined blob still common:
# persona:
#   agent: ./system-prompts/agent.md

runtime:
  max_iterations: 32                # → iterationCap (default ~50 in code paths)
  history_cap: 80                   # dumb prompt tail seatbelt
  max_turns_per_cycle: 15       # reclamp every N user turns (default 15; 1=every turn; 0=freeze)
  action_timeout_sec: 30            # alias: action_timeout
  # mode: normal | autonomous
  # no_session: true
  # ephemeral: true
  # dev_mode: true
  # subagents:
  #   persistence: memory | session

prompt_building:
  runtime_capabilities:             # aliases: available_actions
    input_schemas: disable          # enable|true to inject full JSON schemas
    return_schemas: false
    usage_examples: false
    # legacy aliases: output_schema, examples

# sandbox:     → docs/manifests/sandbox.md
# compaction:  → docs/manifests/compaction.md  (alias: compacting:)

import:
  tools: []                         # bare builtin name | path to tool.yml
  feeds: []
  agents: []                        # bare global name | path to agent.yml | sibling name
  relics: []
  workflows: []
  files: []                         # static prompt modules ONLY
  env: []                           # KEY=value or KEY (from process env)
```

### Field → runtime (high signal)

| YAML | Runtime |
|---|---|
| `name` / `version` / `summary` | `AgentConfig` identity |
| `cognitive_engine.primary.*` | provider, model, temperature, maxTokens |
| `cognitive_engine.fallback.*` | fallbackProvider / fallbackModel |
| `context.harness` | harness text → `<harness><protocol>` |
| `context.system` / system path | system prompt body |
| `context.persona` / `persona.agent` | persona block / legacy system path |
| `runtime.max_iterations` | `iterationCap` |
| `runtime.history_cap` | `historyCap` |
| `runtime.max_turns_per_cycle` | `maxTurnsPerCycle` |
| `runtime.action_timeout_sec` | `actionTimeoutSec` |
| `prompt_building.runtime_capabilities.*` | tool card verbosity |
| `sandbox.*` | `SandboxPolicy` + binds (see sandbox.md) |
| `compaction.*` | `CompactionConfig` (see compaction.md) |
| `import.tools` | tools + schema XML |
| `import.agents` | sub-agents (isolated imports) |
| `import.files` | `__PROMPT_MODULES_XML__` |
| `import.env` | agent env map |

### `sandbox:` (summary — full doc: sandbox.md)

```yaml
sandbox:
  mode: process                     # process | docker | chroot
  image: alpine:3.19                # docker (runtime: legacy alias)
  network: out                      # none | out | full
  readonly: false
  allowed_commands: [ls, cat]       # [] block exec; ["*"] all
  allowed_paths: ["./"]
  allowed_hosts: []                 # [] block web_fetch; ["*"] all
  files: [./seed.txt]               # → /workspace/<basename> LIVE
  bind:
    - ./ctx:/workspace/ctx
    - ./fixtures:/workspace/fixtures:ro
    - { path: ./data, to: /workspace/data, readonly: true }
```

### `compaction:` (summary — full doc: compaction.md)

```yaml
compaction:                         # alias compacting:
  enabled: true
  profile: balanced                 # none|light|balanced|aggressive|archive_first
  trigger:
    context_tokens: 60000           # also 60k / context_window
    context_pct: 0.65
    turns: 15
  cooldown: { min_turns: 2 }
  policy:                           # optional full map
    default: { keep: tail, keep_last: 6, truncate_chars: 2000 }
    tags: { thought: { keep: none }, user: { keep: all } }
    never_drop: [pin, open_ask]
  overrides:                        # on top of profile
    tags: { result: { keep_last: 20 } }
  output:
    mode: summarize_rules           # drop | summarize_rules | summarize_llm
    archive: { enabled: true, sink: file, format: markdown }
  subagents: { inherit: true, child_before_return: true }
```

Absent block = off. Does not replace `history_cap`. UI `/truncate` is unrelated (display only).

---

## Tool (`kind: Tool`)

```yaml
kind: Tool
name: my_tool                       # action name=
version: "1.0"
summary: "…"

runtime: python3                    # python3 | bash | node | builtin
entrypoint: ./src/main.py           # relative to tool.yml
# implementation:
#   input_type: json                # if used by loader

input_schema:
  type: object
  required: [path]
  properties:
    path: { type: string, description: "…" }
    mode: { type: string, enum: [fast, thorough], default: fast }

output_schema:
  type: object
  properties:
    success: { type: boolean }
    result: { type: string }

examples:
  - description: "happy path"
    params: { path: "src/main.cpp", mode: fast }

on_error: abort                     # abort | retry | skip
timeout: 30
max_retries: 0
```

### Script contract

```text
<runtime> <entrypoint> '<json_params>'
→ stdout: one JSON document
→ exit 0 success / non-zero error
→ stderr logged, not returned as tool JSON
```

Python minimal:

```python
#!/usr/bin/env python3
import sys, json
params = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
print(json.dumps({"success": True, "result": "done"}))
```

---

## Feed (`kind: Feed`)

```yaml
kind: Feed
name: my_feed                       # XML-ish feed tag surface
version: "1.0"
summary: "…"
runtime: bash                       # bash | python3 | node | builtin | …
entrypoint: ./src/poll.sh
interval_secs: 60
output_schema:
  type: object
  properties:
    status: { type: string }
```

Contract: no params; JSON stdout; fast; idempotent; non-zero keeps last value.

---

## Relic (`kind: Relic`)

```yaml
kind: Relic
name: my_relic
version: "1.0"
summary: "…"
mode: managed                       # managed | remote | builtin
port: 8200
health_path: /health
# compose_dir / compose_file / env_file / project_name  # managed docker
endpoints:
  store:
    method: POST
    path: /store
    description: "Store data"
  retrieve:
    method: GET
    path: /retrieve
    description: "Retrieve data"
```

Agent call shape (protocol):  
`<action type="relic" name="my_relic" id="r1">{"endpoint":"store",…}</action>`

Managed relics stay up after start. Core session/checkpoint ≠ stdlib relic.

---

## Workflow (`kind: Workflow`)

```yaml
kind: Workflow
name: deploy
version: "1.0"
summary: "…"
# tags: []
# import: { tools: [], agents: [], feeds: [] }

steps:
  - id: build
    type: tool                      # tool | agent | condition | loop | parallel | workflow
    tool: exec
    params: { command: "make build" }
    on_error: abort                 # abort | retry | skip
    max_retries: 0
    timeout: 30

  - id: branch
    type: condition
    condition: "build.success == true"
    then:
      - id: deploy
        type: agent
        agent: builder
    else:
      - id: note
        type: tool
        tool: exec
        params: { command: "echo fail" }
```

---

## Import resolution

```yaml
import:
  tools: [exec, ./tools/x/tool.yml]
  feeds: [system-clock, ./feeds/y/feed.yml]
  agents: [default, ./agents/child/agent.yml, reviewer]
  relics: [artifact_store]
  workflows: [./workflows/deploy.yml]
  files: [./prompts/contract.md]
  env: [API_KEY, DEBUG=1]
```

| Form | Resolution |
|---|---|
| Bare tool name | Builtin catalog via tool registry (must be listed to load) |
| `./…/tool.yml` | Path relative to agent.yml |
| Bare agent name | Global/manifests lookup or sibling module convention |
| Path agent | Load that `agent.yml` as sub-agent |
| Bare relic | `manifests/relics/<name>/` (dispatcher) |
| Path feed/workflow | Load manifest at path |
| `files` entry | Read text → `<module name="…">` in system prompt |
| `env` `KEY=val` | Set; bare `KEY` pull-through from process env |

**Sub-agent isolation:** child does not inherit parent tools. Parent calling child uses `type="agent"` actions; child’s own manifest defines its world.

---

## Builtin catalogs (opt-in via `import.tools`)

Common builtins (verify in tree if unsure):

| Name | Role |
|---|---|
| `exec` | shell |
| `grep` | content search |
| `list` | directory listing |
| `fs_read` / `fs_write` | file IO |
| `json` | JSON helpers |
| `web_fetch` | HTTP fetch (sandbox hosts gate) |
| `sleep` | delay |
| `ask_tool` | human/ask cards |
| `context_pin` / `context_peek` / `context_unpin` | pinned context |

Feeds often referenced by name: `system-clock`, `system-stats`, `working-directory` (if present in tree).

---

## Harness tiers (paths only)

PE judgment → `manifest-expert` / `harness-tuner`. Paths:

| File | Typical use |
|---|---|
| `manifests/harness/small.md` | sub-agents |
| `manifests/harness/default.md` | parents/workers |
| `manifests/harness/medium.md` | more detail |
| `manifests/harness/big.md` | stubborn models |

Wire via `context.harness: <rel path>`.

---

## Load path (runtime wiring)

```
cortex-mk3 -m path/to/agent.yml
  ManifestLoader::loadAgentConfig
  provider + Agent(cfg)
  loadTools / loadSubAgents / loadWorkflows / feeds
  sandbox policy + binds (if sandbox:)
  import.files → prompt modules env
  agent.prompt → buildSystemPrompt
      harness + persona + system + modules
      + action_available (tools/relics/agents)
      + transcript (history_cap / compaction)
```

Dry-run:

```bash
make cortex-mk3          # if binary stale
./cortex-mk3 --dry-run -m path/to/agent.yml
```

---

## Protocol surface (schema-level)

Model emits (CANON): `<thought>` · `<action>` · `<response>` / `<response final="true">`  
Runtime injects: `<result>` · `<context_feed>`

```xml
<action type="tool" name="list" id="ls1" mode="sync">{"path":"."}</action>
<action type="agent" name="reviewer" id="r1">Review src/</action>
<response final="true">Done.</response>
```

| Attr | Notes |
|---|---|
| `type` | `tool\|agent\|relic\|feed\|workflow` |
| `name` | must exist under action_available |
| `id` | unique for the run |
| `mode` | `sync` (default) \| `async` \| `fire_and_forget` |
| `depends_on` | producer ids; piping `${id.field}` |

Full law: `docs/protocol/CANON.md` — not duplicated here.

---

## Anti-patterns (schema/layout)

| Don't | Do |
|---|---|
| Code at module/tool root | `src/<concern>/` |
| Assume builtins always loaded | List in `import.tools` |
| Assume child inherits parent tools | Declare child imports |
| `import.files` as mounts | `sandbox.bind` / `sandbox.files` |
| Absolute paths in YAML | Relative to YAML dir |
| `config.yml` sidecar | Native agent.yml fields |
| God imports (everything) | Narrow lists |
| Invent top-level keys | Ship against docs/loader |

---

## Related skills

| Skill | Use |
|---|---|
| **`manifest-expert`** | Full-stack package design, PE, review, sandbox/compact judgment |
| **`harness-tuner`** | Structural compliance iteration on harness text |
| **`manifest-schemas`** (this) | Exact keys and shapes |

**Rename note:** formerly `mk3-manifest`. Update triggers/docs to `manifest-schemas`.
