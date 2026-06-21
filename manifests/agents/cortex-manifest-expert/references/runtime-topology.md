# Runtime Topology

How a model action becomes a tool call, feed poll, or sub-agent delegation. Read this when the model is producing surprising behavior — tracing the path is the fastest way to find the bug.

## High-level

```
[User prompt]
    ↓
[Agent harness prompt + system + persona + tools + feeds + sub-agents]
    ↓
[LLM call] → [response] (with <action> tags)
    ↓
[Action parser] → dispatch to:
    ├── tool  → ToolRegistry → Tool::execute → result
    ├── agent → SubAgent::prompt → result
    ├── feed  → FeedEngine:
    │   ├── name="<feed>"        → poll (ambient context)
    │   └── name="<feed>.<tool>" → callFeedTool
    └── relic → Reliquary → DockerManagedRelic::handle → HTTP
    ↓
[Result] → fed back to LLM → [next iteration]
```

## Tool dispatch path

```
1. <action type="tool" name="X" id="a1" mode="sync">{...}</action>
2. protocol::parser::ParsedAction {name: "X", type: TOOL, params: {...}}
3. Agent::dispatchTool → dispatch::dispatchTool
4. ToolRegistry::get("X") → Tool instance
5. Tool::execute(args) — native OR script
   ├── native: Tool::executeNative → C++ callback → Json::Value
   └── script: Agent::executeScriptTool → process::run → JSON parse → Json::Value
6. result stored in executedActions_ for dedup
7. JSON-serialized into <result id="a1" status="ok">...</result>
8. Added to LLM context for next turn
```

## Feed dispatch path

```
1. <action type="feed" name="X" id="f1" mode="sync">{...}</action>
2. ParsedAction {name: "X", type: FEED, params: {...}}
3. Agent::dispatchTool sees type=FEED → dispatch::dispatchFeed
4. dispatch::dispatchFeed:
   ├── name contains '.'  → split into feed + tool → callFeedTool
   │   ├── feed not found → success=false, error="Unknown feed: ..."
   │   ├── tool not found → success=false, error="Unknown tool: ..."
   │   └── tool found     → callTool → process::run → JSON result
   └── no '.'            → poll one feed → FeedResult
       ├── script output  → JSON parsed → "summary" in next turn's <feeds> block
       └── builtin feed   → C++ pollFn → same
```

## Relic dispatch path

```
1. <action type="relic" name="X" id="r1" mode="sync">{"endpoint":"...","key":"value"}</action>
2. ParsedAction {name: "X", type: RELIC, params: {...}}
3. Agent::dispatchTool sees type=RELIC → dispatch::dispatchRelic
4. dispatch::dispatchRelic:
   └── Reliquary::instance().dispatch("X", endpoint, params)
       └── DockerManagedRelic::handle(endpoint, params)
           ├── mode="remote"   → curl endpoint directly
           └── mode="managed"  → ensureContainerUp() → curl http://localhost:<port>/<endpoint>
5. HTTP response parsed as JSON if possible
6. {success, data, error?} returned
```

## Sub-agent dispatch path

```
1. <action type="agent" name="X" id="a1" mode="sync" ephemeral="true">instruction</action>
2. ParsedAction {name: "X", type: AGENT, params: {instruction: "..."}}
3. Agent::dispatchTool sees type=AGENT → dispatch::dispatchAgent
4. dispatch::dispatchAgent:
   └── delegate(invocation) → SubAgent::prompt
       ├── ephemeral=true  → fresh session, no persistence
       └── ephemeral=false → child session ID derived from parent
5. Sub-agent runs its own iteration loop (LLM call, action dispatch, ...)
6. Final <response> text returned
```

## Workflow dispatch path

```
1. <action type="tool" name="workflow" id="w1" mode="sync">{"workflow":"X","params":{...}}</action>
   (or programmatic call from another workflow)
2. Agent::dispatchTool dispatches the `workflow` tool → Agent::executeWorkflow
3. Agent::executeWorkflow:
   ├── builds WorkflowRuntime callbacks (executeTool, executeAgent, executeWorkflow)
   ├── fetches the workflow by name (WorkflowEngine::getCached)
   └── WorkflowEngine::execute(workflow, runtime, params)
4. WorkflowEngine processes each step in order:
   ├── type=tool      → rt.executeTool(tool, resolved_params)
   ├── type=agent     → rt.executeAgent(WorkflowAgentInvocation{...})
   ├── type=condition → eval condition, dispatch then/else steps
   ├── type=parallel  → std::async fan-out, collect results
   └── type=workflow  → rt.executeWorkflow(workflow_name, params)  (recursive)
5. Result: WorkflowResult {success, stepIds, outputs, diagnostics, error, ...}
```

## Prompt assembly

```
At agent startup, buildSystemPrompt assembles:

<harness>    from context.harness (default: manifests/harness/default.md)
<system>     from context.system  (default: manifests/system/default.md)
<persona>    from context.persona (default: manifests/persona/default.md)
<info name="X" version="Y"/>   from agent manifest
<action_available>  block with all imported tools/feeds/sub-agents
  ├── <tools>      list of ToolDef-derived descriptions + JSON schemas
  ├── <feeds>      list of feed names + tool specs (from <feed>.<tool> entries)
  └── <sub_agents> list of sub-agent metadata (no full prompts)
<manifest_count>  counts of each kind
<cwd>             working directory
</system>

<dynamic_context>  block refreshed per turn (bottom-loaded for cache stability)
  ├── <feeds>      poll results for each feed
  └── other context additions
</dynamic_context>
```

## Per-turn state (must be reset)

At the start of every prompt, reset:

- `agentDone` — terminal flag
- `firstToken` — first-token tracking
- `lastEventCount` — transcript event count
- Snapshot fields (`snapEvents`, `snapResponse`, `snapDirty`)

Without this reset, the second query hard-blocks because the transcript thinks it's already complete.

## Substrate: process::run

All shell-out paths (feed poll, feed tool, tool script, workflow step exec, ensureBuilt, Docker compose) route through `process::run` (in `src/utils/process.hpp`):

- Per-call env map (no global setenv leak between concurrent calls)
- 30s default timeout (overridable per spec)
- 1MB stdout / 64KB stderr cap (overridable)
- Exit code, signal, timedOut, truncated fields
- Shell mode (`sh -c cmd`) or argv mode (direct exec)

When in doubt, check `process::run` is being used. Old `popen` paths may linger in POC code; the priority is to migrate them as you touch them.