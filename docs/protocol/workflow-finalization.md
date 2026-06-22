# Draft: Workflow finalization + cross-manifest integration

**Status:** DRAFT — brainstorming + prototyping, no code
**Scope:** finalize the workflow manifest, expand step types, integrate fully with agents/tools/feeds/relics
**Companion:** current spec at `manifests/workflows/workflow_spec.yml`, engine at `src/workflows/workflow_engine.hpp`

---

## 0. Why this draft exists

The current workflow engine works for the basic case: a linear sequence of tool/agent steps with simple on_error handling. But the user said workflows should be "very powerful and versatile" — and right now they're not. Gaps:

1. **Limited step types.** 6 (tool, agent, condition, loop, parallel, workflow). Missing: human-in-loop, checkpoint, relic, feed, emit, map/reduce, try/catch, switch.
2. **No human-in-the-loop.** Workflows that need a human pause, confirmation, or decision can't be expressed.
3. **No state persistence.** Long workflows can't be checkpointed and resumed; failures lose all progress.
4. **No cross-manifest integration patterns.** Agents can call workflows (via `type: workflow`), but the integration is shallow. Tools can't spawn workflows, relics can't be workflows, feeds can't drive workflow state.
5. **Limited observability.** No per-step timing, no cost tracking, no trace export.
6. **Limited control flow.** No `try`/`catch`, no `return`, no `break`/`continue`, no `parallel_race` / `parallel_join` semantics.

This draft proposes:
- **§1** Current state (what works, what's incomplete)
- **§2** New step types (8 additions)
- **§3** Better control flow (try/catch, return, break, race, join)
- **§4** Cross-manifest integration patterns
- **§5** State persistence and resume
- **§6** Observability (per-step metrics, traces)
- **§7** Schema validation
- **§8** Real-world examples
- **§9** Implementation slices
- **§10** Open questions

---

## 1. Current state

### What works
- 6 step types: `tool`, `agent`, `condition`, `loop`, `parallel`, `workflow` (sub-workflow)
- `on_error: abort | skip | continue`
- `max_retries`, `timeout` per step
- Variable interpolation: `${step_id.field}`, `${input.x}`
- Condition expressions: `==`, `!=`, `>=`, `<=`, `>`, `<`
- Workflows can be loaded by name and called from agents
- The `bloated` agent demonstrates an end-to-end workflow (review → deploy)

### What's incomplete or weak
- No human-in-the-loop. Can't pause for confirmation.
- No checkpointing. Long workflows can't be resumed.
- No try/catch. Single-step error handling only.
- No map/reduce. Can't iterate a step over a list.
- No race/join. Parallel is fire-and-forget; can't wait for first to complete.
- No feed/relic/emit step types.
- No per-step cost/timing metrics in the result.
- No output schema. Outputs are JSON-blob; consumers can't validate.
- No workflow inheritance. Can't `extends: base-workflow`.
- The "workflow" step type only calls via the runtime callback; no way to pass sub-workflow params explicitly.

---

## 2. New step types (8 additions)

### 2.1 `human` — pause for user input or confirmation

**Motivation:** Today, no way for a workflow to ask the user. The user said workflows should be "versatile" — sometimes you need a human in the loop.

**Syntax:**
```yaml
- id: confirm_deploy
  type: human
  prompt: "About to deploy ${input.commit} to ${input.environment}. Continue? (y/n)"
  default: y
  timeout_sec: 60         # auto-default after N seconds (optional)
  response_var: confirmed  # store user input in this var
```

**Behavior:** The runtime pauses the workflow, displays the prompt in the TUI, and waits for user input. The user's response is stored in `confirmed`. If `timeout_sec` elapses, the `default` value is used.

**Failure mode:** If the user declines (e.g. responds "n"), the step is treated as failed; `on_error` decides what to do next.

### 2.2 `checkpoint` — save state for resume

**Motivation:** Long workflows (deploy pipelines, multi-day migrations) need resume support. If the process crashes, the user should be able to restart and pick up where it left off.

**Syntax:**
```yaml
- id: after_review
  type: checkpoint
  state: ${steps.review.outputs}    # what to checkpoint
  message: "Review complete, ready to deploy"
```

**Behavior:** The runtime serializes the workflow state to disk. On resume, the user can restart the workflow with `--resume-from=<checkpoint-id>`, and the workflow jumps to the next step.

**Open question:** Where do checkpoints live? Per-session? Per-workflow? See §5.

### 2.3 `relic` — call a relic (e.g. database, deployment service)

**Motivation:** Today, workflows can call tools and agents, but not relics. The `bloated` agent demonstrates a `deployment-relic` that's a Flask service. Workflows should be able to call relics directly.

**Syntax:**
```yaml
- id: check_relic_health
  type: relic
  relic: deployment
  action: status              # name of the relic action
  params:
    service: api-server
  on_error: skip
```

**Behavior:** The runtime calls `relics[name].action` with the given params. The relic's response is stored under the step's output.

**Why this matters:** Relics are stateful services (Postgres, Redis, Docker containers). Workflows that orchestrate infrastructure need direct relic access.

### 2.4 `feed` — read a feed

**Motivation:** Feeds are external data sources (logs, dashboards, metrics). Workflows that react to feed state (e.g. "deploy only if error rate is below 1%") need to read feeds.

**Syntax:**
```yaml
- id: get_metrics
  type: feed
  feed: prometheus-metrics
  query:
    metric: error_rate
    window: 5m
```

**Behavior:** The runtime reads the feed with the given query and stores the result. The result can be referenced in subsequent steps or in conditions.

### 2.5 `emit` — emit a custom event

**Motivation:** Workflows need to communicate with the outside world (notify a Slack channel, log a milestone, etc.) without calling a tool that has side effects.

**Syntax:**
```yaml
- id: notify_started
  type: emit
  event: "deploy.started"
  payload:
    commit: ${input.commit}
    environment: ${input.environment}
```

**Behavior:** The runtime emits the event. Subscribers (feeds, other workflows, external systems) can listen. The workflow continues immediately.

### 2.6 `map` — apply a step to each item in a list

**Motivation:** Today, no way to iterate a step over a list. The user has to use a `loop` with a manual counter, which is error-prone.

**Syntax:**
```yaml
- id: review_all_files
  type: map
  over: ${steps.find_files.outputs.files}   # the list
  as: file
  step:
    type: agent
    agent: code-quality
    params:
      target: ${file}
```

**Behavior:** The runtime iterates the step over each item in the list. The variable `file` is bound to the current item. Outputs are collected as a list under the parent step's id.

**Why this matters:** A review workflow over N files, a deploy over N services, a test run over N test cases — all need this.

### 2.7 `reduce` — aggregate over a list

**Motivation:** Map produces a list; you need to combine the results. Reduce does this.

**Syntax:**
```yaml
- id: total_findings
  type: reduce
  over: ${steps.review_all_files.outputs}
  initial: 0
  step:
    type: tool
    tool: exec
    params:
      command: "echo $(( ${acc} + ${item.count} ))"
```

**Behavior:** The runtime runs the step with `acc` (accumulator) and `item` (current). The result becomes the new `acc`. Final accumulator is the step's output.

### 2.8 `switch` — multi-way branch

**Motivation:** `condition` is binary (then/else). Sometimes you need N-way branching.

**Syntax:**
```yaml
- id: route_by_environment
  type: switch
  on: ${input.environment}
  cases:
    - value: production
      steps: [...production-deploy-steps...]
    - value: staging
      steps: [...staging-deploy-steps...]
    - value: dev
      steps: [...dev-deploy-steps...]
  default: [...fallback-steps...]
```

**Behavior:** The runtime matches `input.environment` against each `case.value` and executes the matching `steps`. If no match, the `default` block runs.

---

## 3. Better control flow

### 3.1 `try_catch` — error recovery around a step or block

**Motivation:** Today, `on_error` is per-step and binary. Real workflows need try/catch around a block.

**Syntax:**
```yaml
- id: deploy_with_rollback
  type: try_catch
  body:
    - id: deploy
      type: tool
      tool: platform-deploy
      params: { environment: production, commit: ${input.commit} }
    - id: verify
      type: tool
      tool: exec
      params: { command: "curl -f https://api.example.com/health" }
  catch:
    - id: rollback
      type: tool
      tool: platform-deploy
      params: { action: rollback, commit: ${input.commit} }
  finally:
    - id: notify
      type: emit
      event: "deploy.completed"
      payload: { commit: ${input.commit} }
```

**Behavior:** The runtime runs `body`; if any step fails, `catch` runs. `finally` always runs (success or failure).

### 3.2 `return` — short-circuit return

**Motivation:** A step deep in a workflow that has all the info it needs to return should be able to short-circuit.

**Syntax:**
```yaml
- id: short_circuit
  type: return
  value: { status: "skipped", reason: "no changes" }
```

**Behavior:** The workflow exits immediately, returning `value` as the workflow's output.

### 3.3 `break` and `continue` (within loops)

**Motivation:** Today, a loop with `condition` is the only escape. `break` and `continue` are clearer.

**Syntax:** Set a special var in a step:
```yaml
- id: process_items
  type: loop
  body:
    - id: check
      type: condition
      condition: "item.empty"
      # To break, set __break__ to true in the step's output
    - id: step1
      type: tool
      ...
      on_complete:
        set:
          __break__: true   # exits the loop early
```

**Behavior:** Setting `__break__: true` exits the loop. Setting `__continue__: true` skips to the next iteration.

### 3.4 `parallel_race` — first to complete wins

**Motivation:** Today, `parallel` waits for ALL to complete. Sometimes you want "race" — first to finish wins, others are cancelled.

**Syntax:**
```yaml
- id: fetch_from_any_mirror
  type: parallel_race
  steps:
    - id: primary
      type: tool
      tool: web_fetch
      params: { url: "https://primary.example.com/data" }
    - id: mirror1
      type: tool
      tool: web_fetch
      params: { url: "https://mirror1.example.com/data" }
    - id: mirror2
      type: tool
      tool: web_fetch
      params: { url: "https://mirror2.example.com/data" }
```

**Behavior:** All three fire. The first to complete wins; the other two are cancelled. The output is the winning step's result.

### 3.5 `parallel_join` — fail if any fails

**Motivation:** Today, `parallel` ignores individual failures (they're in `outputs`, but the parent step succeeds). Sometimes you want strict join.

**Syntax:**
```yaml
- id: deploy_to_all_regions
  type: parallel_join
  steps: [...]
  on_any_error: abort
```

**Behavior:** All must succeed. If any fails, the parent step fails (and `on_error` decides what to do).

---

## 4. Cross-manifest integration patterns

### 4.1 Workflows ↔ Agents

**Today:** A workflow step can call an agent (`type: agent`). An agent can call a workflow via `<action type="tool" name="workflow_run">`.

**Proposed:** A workflow can also BE the entrypoint of an agent. When the user invokes the agent, the workflow starts.

```yaml
# In agent.yml
workflows:
  - code-review-workflow     # the default workflow for this agent
```

**Why:** Some agents are pure orchestrators. They don't have a chat loop — they just run a workflow.

### 4.2 Workflows ↔ Tools

**Today:** A workflow calls tools. Tools can spawn workflows (e.g. `workflow_run` is a tool that takes a workflow name and params).

**Proposed:** Tools can declare that they are "workflow entrypoints" — they're invoked by name and start a workflow. Useful for `code-review` and `deploy` tools that the user invokes with a target/commit.

### 4.3 Workflows ↔ Relics

**Today:** Workflows can call tools. They can't call relics directly. (See §2.3.)

**Proposed:** Add `type: relic` as a step type. The runtime calls `relics[name].action`. This is the natural way to do `platform-deploy` and `db-migrate` as workflows.

### 4.4 Workflows ↔ Feeds

**Today:** No direct integration. Feeds are LLM-only.

**Proposed:** Add `type: feed` as a step type. The runtime reads a feed. Workflows can be feed-driven (e.g. "react to feed events" or "wait for feed state").

### 4.5 Relics as workflows

**Today:** Relics are imperative (start a container, run a command, etc.). State is implicit.

**Proposed:** A relic can be a workflow — declarative. The relic's "ensure up" is a workflow that checks, builds, starts, and verifies. The same workflow runs on cold start AND on user request. This makes relics reproducible.

**Example:**
```yaml
kind: Relic
name: api-server
spec:
  type: workflow      # the relic IS a workflow
  workflow: ensure-api-server-up
  inputs:
    image: "myapp:1.0"
    port: 8080
```

The `ensure-api-server-up` workflow:
```yaml
kind: Workflow
name: ensure-api-server-up
steps:
  - id: check
    type: tool
    tool: exec
    params: { command: "docker ps -q -f name=api-server" }
  - id: build
    type: condition
    condition: "check.output == ''"
    then:
      - id: build
        type: tool
        tool: exec
        params: { command: "docker build -t myapp:${input.image} ." }
      - id: start
        type: tool
        tool: exec
        params: { command: "docker run -d -p ${input.port} --name api-server myapp:${input.image}" }
  - id: verify
    type: tool
    tool: exec
    params: { command: "curl -f http://localhost:${input.port}/health" }
```

This makes the relic's behavior inspectable, version-controlled, and replayable.

### 4.6 Feeds as workflow triggers

**Today:** Feeds are LLM-only.

**Proposed:** A feed can trigger a workflow on state change. E.g. `prometheus-feed` triggers `incident-response-workflow` when error rate > 5%.

```yaml
kind: Feed
name: prometheus-feed
triggers:
  - workflow: incident-response-workflow
    when: "metric('error_rate') > 0.05"
    inputs:
      severity: high
```

This makes feeds an active component, not just a passive data source.

---

## 5. State persistence and resume

### 5.1 Checkpoint schema

A checkpoint is a JSON file:

```json
{
  "workflow": "deploy-pipeline",
  "checkpoint_id": "after-review",
  "created_at": "2026-06-22T18:00:00Z",
  "inputs": { "commit": "abc123", "environment": "production" },
  "completed_steps": ["review"],
  "step_outputs": { "review": { "...": "..." } },
  "next_step": "deploy",
  "version": "1.0"
}
```

### 5.2 Storage

Checkpoints live in `.cortex/checkpoints/<workflow-name>/<checkpoint-id>.json`. They survive process restarts.

### 5.3 Resume syntax

```bash
cortex-mk3 --workflow deploy-pipeline --resume-from after-review
```

Or in the agent manifest:
```yaml
workflows:
  - name: deploy-pipeline
    resume_from: after-review
```

### 5.4 Auto-checkpoint

Workflows can declare `auto_checkpoint: true` — the runtime saves a checkpoint after every step. Useful for long workflows that may crash.

---

## 6. Observability

### 6.1 Per-step metrics

Each step in the result has:

```json
{
  "id": "review",
  "type": "agent",
  "started_at": "2026-06-22T18:00:00Z",
  "elapsed_ms": 12500,
  "tokens": 4500,
  "cost_usd": 0.012,
  "success": true,
  "output": { "...": "..." }
}
```

### 6.2 Trace export

A workflow execution can be exported as OpenTelemetry or a JSON trace file. Useful for debugging and visualization.

```bash
cortex-mk3 --workflow deploy-pipeline --trace-out ./trace.json
```

### 6.3 Streaming progress

Long workflows emit progress events that the TUI can render as a progress bar. The user sees "Step 3/8: review" instead of just "Running...".

---

## 7. Schema validation

### 7.1 Input schema

Workflows declare an `input_schema` (already used in the bloated example):

```yaml
input_schema:
  type: object
  required: [commit, environment]
  properties:
    commit:
      type: string
    environment:
      type: string
      enum: [staging, production]
```

The runtime validates inputs before starting. If invalid, the workflow fails with a clear error.

### 7.2 Output schema (NEW)

```yaml
output_schema:
  type: object
  required: [success, review_count]
  properties:
    success:
      type: boolean
    review_count:
      type: integer
```

The runtime validates the output. If invalid, the workflow fails (or warns, depending on `strict`).

### 7.3 Step params schema (NEW)

Each step can declare its own input schema. The runtime validates `params` against the schema before running.

```yaml
- id: deploy
  type: tool
  tool: platform-deploy
  params_schema:
    type: object
    required: [commit, environment]
    properties:
      commit: { type: string }
      environment: { type: string, enum: [staging, production] }
  params:
    commit: ${input.commit}
    environment: ${input.environment}
```

---

## 8. Real-world examples

### 8.1 Production deploy with rollback

```yaml
kind: Workflow
name: production-deploy
summary: Deploy to production with full review, health checks, and rollback

input_schema:
  type: object
  required: [commit]
  properties:
    commit: { type: string }

steps:
  - id: review
    type: agent
    agent: code-quality
    params: { target: "${input.commit}" }
    on_error: abort

  - id: check_blockers
    type: condition
    condition: "review.count > 0"
    then:
      - id: confirm_with_blockers
        type: human
        prompt: "Found ${review.count} BLOCKER(s). Continue anyway? (y/n)"
        default: n
        timeout_sec: 30
      - id: gate
        type: condition
        condition: "confirmed == 'n'"
        then:
          - id: skip
            type: return
            value: { status: "cancelled", reason: "BLOCKERs present" }

  - id: notify_start
    type: emit
    event: "deploy.started"
    payload: { commit: "${input.commit}" }

  - id: deploy
    type: try_catch
    body:
      - id: deploy_to_production
        type: tool
        tool: platform-deploy
        params:
          commit: "${input.commit}"
          environment: production
      - id: health_check
        type: tool
        tool: exec
        params: { command: "curl -f https://api.example.com/health" }
    catch:
      - id: rollback
        type: tool
        tool: platform-deploy
        params:
          action: rollback
          commit: "${input.commit}"
      - id: notify_failure
        type: emit
        event: "deploy.failed"
        payload: { commit: "${input.commit}" }
      - id: rethrow
        type: return
        value: { status: "failed", reason: "deploy or health check failed" }

  - id: notify_success
    type: emit
    event: "deploy.succeeded"
    payload: { commit: "${input.commit}" }

  - id: checkpoint
    type: checkpoint
    state: { commit: "${input.commit}", deployed_at: "now" }
    message: "Production deploy of ${input.commit} complete"

output_schema:
  type: object
  required: [status]
  properties:
    status: { type: string, enum: [succeeded, failed, cancelled] }
    reason: { type: string }
```

### 8.2 Multi-file review with map

```yaml
kind: Workflow
name: multi-file-review
input_schema:
  type: object
  required: [files]
  properties:
    files:
      type: array
      items: { type: string }

steps:
  - id: review_each
    type: map
    over: "${input.files}"
    as: file
    step:
      type: agent
      agent: code-quality
      params: { target: "${file}", focus: "race conditions" }

  - id: total_findings
    type: reduce
    over: "${steps.review_each.outputs}"
    initial: 0
    step:
      type: tool
      tool: exec
      params: { command: "echo $(( ${acc} + ${item.findings_count} ))" }

  - id: write_report
    type: tool
    tool: fs_write
    params:
      path: "./review-report.md"
      content: |
        # Multi-file review

        Total findings: ${steps.total_findings.output}

        Per-file:
        ${steps.review_each.outputs}
```

### 8.3 Feed-driven incident response

```yaml
kind: Workflow
name: incident-response
triggers:
  - feed: prometheus-metrics
    when: "metric('error_rate_5m') > 0.05"

input_schema:
  type: object
  required: [severity]
  properties:
    severity: { type: string, enum: [low, medium, high] }

steps:
  - id: check_severity
    type: switch
    on: "${input.severity}"
    cases:
      - value: high
        steps:
          - id: page_oncall
            type: emit
            event: "pagerduty.trigger"
            payload: { severity: high, service: api }
          - id: start_rollback
            type: tool
            tool: platform-deploy
            params: { action: rollback, environment: production }
      - value: medium
        steps:
          - id: notify_slack
            type: emit
            event: "slack.notify"
            payload: { channel: "#incidents", message: "Error rate elevated" }
      - value: low
        steps:
          - id: log
            type: tool
            tool: exec
            params: { command: "echo 'Low severity incident logged' >> /var/log/incidents" }
```

---

## 9. Implementation slices

Each slice is atomic, has tests, and can be shipped independently.

| # | Slice | Effort | Impact |
|---|---|---|---|
| 1 | **Schema validation** (input + output + step params) | ~200 LOC | High (errors caught early) |
| 2 | **New step types: `human`, `relic`, `feed`, `emit`** | ~300 LOC | High (unlocks new workflows) |
| 3 | **New step types: `map`, `reduce`, `switch`** | ~200 LOC | Medium (replaces loop+counter hacks) |
| 4 | **New step types: `checkpoint`, `return`** | ~150 LOC | High (long workflows) |
| 5 | **`try_catch` step type** | ~150 LOC | High (clean error handling) |
| 6 | **`parallel_race` / `parallel_join`** | ~100 LOC | Medium (multi-region deploys) |
| 7 | **Workflow as agent entrypoint** (`agent.yml.workflows:`) | ~100 LOC | Medium (some agents are pure orchestrators) |
| 8 | **Relic as workflow** (declarative infrastructure) | ~150 LOC | High (reproducible relics) |
| 9 | **Feed as workflow trigger** | ~200 LOC | High (reactive workflows) |
| 10 | **State persistence + resume** (auto-checkpoint, `--resume-from`) | ~250 LOC | High (long workflows) |
| 11 | **Per-step metrics** (tokens, cost, timing) | ~150 LOC | Medium (observability) |
| 12 | **Trace export** (OpenTelemetry, JSON) | ~200 LOC | Medium (debugging) |
| 13 | **Workflow inheritance** (`extends: base-workflow`) | ~150 LOC | Low (composition) |

**Proposed ship order:** 1, 2, 5, 4, 7 in v1 (the basics + try/catch + checkpoint). 3, 6 in v1.1 (map/reduce/race). 8, 9, 10 in v2 (the deep integration). 11, 12, 13 in v3 (observability + composition).

---

## 10. Open questions for the user

1. **New step types: which 3-4 to ship first?** My pick: `human`, `try_catch`, `checkpoint`, `map` (the four most-requested).
2. **Schema validation: required or opt-in?** My pick: required (errors caught early beats the runtime cost).
3. **Checkpoint storage: per-session or per-workflow?** My pick: per-workflow (`.cortex/checkpoints/<name>/<id>.json`).
4. **Auto-checkpoint default: true or false?** My pick: false (opt-in; users add `auto_checkpoint: true` for long workflows).
5. **Workflow as agent entrypoint: a separate field or `kind: WorkflowAgent`?** My pick: `agent.yml.workflows: [name]` (simpler, leverages existing kind: Agent).
6. **Relic as workflow: replaces imperative relics or augments them?** My pick: augments — both can coexist; declarative is opt-in.
7. **Feed triggers: dedicated field or generic webhook system?** My pick: dedicated `triggers:` field on Feed.
8. **Trace format: OpenTelemetry or JSON?** My pick: JSON for v1 (simpler), OpenTelemetry adapter in v2.
9. **Workflow inheritance syntax: `extends:` or `imports:`?** My pick: `extends:` (more explicit; `imports:` is for tools/relics).
10. **Breaking changes to existing workflows?** My pick: zero — all new step types are additive. Existing workflows keep working.

Once these are answered, I can promote the prioritized slices to design docs and then to code in atomic commits.
