# Workflow Manifest Schema

**Authored at:** `manifests/workflows/<name>.yml`

**Parsed by:** `src/workflows/workflow_engine.hpp` (load + step parsing), `src/workflows/workflow.hpp` (data structures)

**Executed by:** `src/workflows/workflow_engine.hpp::execute` — calls the registered `WorkflowRuntime` callbacks for tools/agents/workflows.

## Required fields

```yaml
kind: Workflow
name: <snake_case>
version: "<semver>"
summary: "<one-line>"
```

## Optional: imports

```yaml
import:
  tools: [exec, fs_read]          # tool names; resolved against the tool registry
  relics: []                      # reserved; not yet implemented
```

## Steps

A workflow is a list of `steps`. Each step has an `id`, a `type`, and kind-specific fields.

### Reserved step fields (NOT user params)

These are reserved and read from the top level of the step:

- `id` (required) — unique within the workflow
- `type` — `tool`, `agent`, `condition`, `loop`, `parallel`, `workflow`
- `name` — display name (optional)
- `tool` — tool name (for `type: tool`)
- `agent` — agent name (for `type: agent`)
- `condition` — expression (for `type: condition`)
- `workflow` — sub-workflow name (for `type: workflow`)
- `on_error` — `abort` (default), `skip`, or `continue`
- `max_retries` — default 0
- `timeout` — default 30s

Everything else belongs under `params:`.

### Params block

```yaml
- id: my_step
  type: tool
  tool: exec
  params:
    command: "echo hello"
    timeout: 5
```

The `params` are forwarded to the tool/agent callback. For agent steps, params can include:

- `instruction` (or `query`) — the prompt for the sub-agent
- `ephemeral` (bool) — request a non-persistent child session
- `dump_context` (bool) — return the child's trace context alongside the response

```yaml
- id: think
  type: agent
  agent: planner
  params:
    instruction: "outline a plan"
    ephemeral: true
    dump_context: true
```

### Step types

```yaml
# type: tool
- id: prepare
  type: tool
  tool: exec
  params: { command: "..." }

# type: agent
- id: plan
  type: agent
  agent: planner
  params: { instruction: "..." }

# type: condition
- id: gate
  type: condition
  condition: "${prev_step.success} == true"
  then:
    - { id: a, type: tool, tool: ..., params: ... }
  else:
    - { id: b, type: tool, tool: ..., params: ... }

# type: parallel
- id: fanout
  type: parallel
  steps:
    - { id: a, type: tool, tool: x, params: ... }
    - { id: b, type: tool, tool: y, params: ... }
    - { id: c, type: tool, tool: z, params: ... }

# type: workflow (recursive)
- id: sub
  type: workflow
  workflow: child_workflow_name
  params: { ... }
```

## Variable resolution

`${input.name}` and `${step_id.field}` are resolved in string-typed params before dispatch.

```yaml
params:
  command: "wc -l <<'_EOF'\n${read_step.output}\n_EOF"
```

Use heredocs for shell metacharacters; inline `${var}` substitution is unsafe for shell.

## Examples

- `manifests/workflows/code-review.yml` — multi-step tool+agent workflow
- `src/testing/fixtures/skip-error.yml`, `abort-error.yml` — on_error policy
- `src/testing/fixtures/agent-modifiers.yml` — ephemeral + dump_context
- `src/testing/fixtures/parallel-symbols.yml` — parallel fanout

## Common mistakes

1. **Putting `instruction:` / `ephemeral:` / `dump_context:` at the top of a step** — they belong under `params:`. The top level is reserved for step routing fields.
2. **Step `id` collisions** — duplicate IDs overwrite symbols; the second win. The parser doesn't enforce uniqueness.
3. **`type: parallel` with steps that share state** — parallel tasks each get a `const` snapshot of the symbols map (slice 6d fix). They cannot write to the shared map; they can only return results.
4. **Heredoc with `<<_EOF` and no trailing `_EOF`** — YAML escape rules will eat the heredoc body. Use a quoted scalar or block scalar.
5. **Forgetting `import.tools:` for a tool the workflow uses** — the workflow loads but the tool callback is null and the step errors with "no tool executor configured".
6. **Recursive `type: workflow` without a max-depth guard** — the engine doesn't have a depth limit. Cycles will infinite-loop. Audit it before running.