# Broken: workflow step with params at the wrong level

## The broken workflow

```yaml
kind: Workflow
name: bad_params
version: "1.0"
summary: "Bug: instruction and ephemeral at top of step instead of under params:"

steps:
  - id: think
    type: agent
    agent: planner
    instruction: "outline a plan"     # ← bug
    ephemeral: true                   # ← bug
    dump_context: true                # ← bug
```

## What goes wrong

The workflow loader only looks under `params:` for step-execution data. The top-level `instruction`, `ephemeral`, `dump_context` are silently ignored.

Result: the planner runs with default `instruction: "Execute task"`, no ephemeral flag, no dump_context. The behavior is silently different from what the author intended.

## The fix

```yaml
steps:
  - id: think
    type: agent
    agent: planner
    params:
      instruction: "outline a plan"
      ephemeral: true
      dump_context: true
```

## Why this is confusing

The top-level step fields (`id`, `type`, `agent`, `tool`, `condition`, ...) are reserved. Anything else goes under `params:`. The author probably saw the top-level reserved fields and assumed other fields also live at the top.

## Detection

The workflow runs without errors, but the planner does the wrong thing. Always check the step output for the actual instruction that was sent.

## Tests

`src/testing/workflow_engine_test.cpp::test_agent_step_propagates_modifiers_to_callback` — verifies that `ephemeral` and `dump_context` reach the callback via `WorkflowAgentInvocation`.

## Related bug (fixed in slice 6b)

Workflow YAML before slice 6b didn't have `ephemeral` / `dump_context` propagation at all. The fix added the struct + parser plumbing + test.