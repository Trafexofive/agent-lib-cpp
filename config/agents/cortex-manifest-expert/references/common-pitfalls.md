# Common Pitfalls

The audit (`docs/TICKETS.md` → audit artifact) and live-test history surfaced a set of recurring issues. Read this before authoring a new manifest.

## MiniYaml list-of-maps parser quirk

**Symptom:** A block-style list item's first `key: value` is not accessible via `ManifestYaml::get(item, key)`.

**Example:**

```yaml
tools:
  - name: refresh
    description: Reload
  - name: set
    description: Write
```

`ManifestYaml::get(tools[0], "name")` returns `""` (empty), not `"refresh"`. The `name` is on `entry.key`/`entry.value`, not as a child.

**Workaround:** The feed parser has a fallback:

```cpp
tool.name = ManifestYaml::get(entry, "name");
if (tool.name.empty() && entry.key == "name")
    tool.name = entry.value;
```

**Defensive authoring:** Use an explicit child `name:` key for any block-style list item whose name is consumed via `ManifestYaml::get`:

```yaml
- name: refresh                   # entry.key == "name", value == "refresh"  (works via fallback)
  description: Reload

- tool:                            # different first key — entry.key == "tool", value == "..."
    name: refresh                  # child key, "name" lives as a child   (works directly)
    description: Reload
```

If you control the consumer, prefer the `entry.key`-fallback pattern (the feed parser) or write a consumer that also has the fallback.

**Where this bit us:** Tool manifest `examples:` parsing (not yet fixed), feed `tools:` (fixed in slice 3).

## `isPathImport` heuristic

**Symptom:** A `feeds:` (or `agents:`) entry is treated as a builtin name instead of a path.

**Rule (from `src/core/manifest_loader.hpp:57`):**

```cpp
if (raw.size() >= 4 && raw.substr(raw.size() - 4) == ".yml") return true;
if (raw[0] == '/') return true;
if (raw.size() >= 2 && raw[0] == '.' && (raw[1] == '/' || raw[1] == '.')) return true;
return false;
```

So a path must end in `.yml`, be absolute, or start with `./` / `..`. Anything else (including `builtin/exec` style) is a name.

**Defensive authoring:** Always use `./` for relative paths:

```yaml
import:
  feeds:
    - ./morpheus_dashboard/feed.yml    # path
    - system_clock                     # name (builtin)
```

## Empty feed output is now a load failure

**Change:** Slice 7 made empty output a hard load failure. Opt-in with `allow_empty: true`.

**Symptoms of accidental emptiness:**
- Feed manifest parses, but the feed isn't registered
- `[manifest] feed path not found` or similar error in the agent's startup log
- The model has no `<morpheus_dashboard>` block in its prompt

**Defensive authoring:** If your feed is allowed to report nothing, set `allow_empty: true` explicitly. Most empty-output cases are real bugs (script crashed, missing env var, etc.) and should fail loudly.

## `working_directory.touch` no longer exists

**Change:** Slice 1 (`d40be0b`) removed the no-op `touch` tool from the `working_directory` feed. The `working_directory` feed has only `refresh` now.

**Defensive authoring:** Don't model a feed tool on a removed demo. Check the current feed manifest under `config/agents/<agent>/` or `manifests/feeds/`.

## Unknown dotted feed tools hard-error

**Change:** Slice 2 (`4f79ca8`) made `<action type="feed" name="feed.unknown_tool">` a hard error with `success: false` and structured `feed` / `tool` / `error` fields. The old behavior silently fell back to polling the literal dotted name.

**Defensive authoring:** If the model is calling a feed tool that doesn't exist, the error message tells you the feed name and the tool name. Use that to fix the manifest or the model prompt.

## Manifest-declared feed tools are not advertised until wired

**Change:** Slice 3 (`b185281`) added parsing of the `tools:` block. Slice 5 (`408a785`) wired them to invocation handlers. The wiring happens automatically inside `loadFeedManifest`. If the wiring fails (e.g., C++-registered handler collides), the manifest handler is skipped — but the C++ handler is preserved.

**Defensive authoring:** If your feed tool is missing from `<action_available>`:
1. Check that the manifest is being loaded (look for it in the agent's startup log)
2. Check that the tool has a unique name (C++-registered handlers win on collision)
3. Check that the entrypoint script is executable (`chmod +x`)

## Workflow step params vs top-level fields

**Change:** Slice 6b (`96ea8cc`) made `ephemeral` and `dump_context` propagate through `WorkflowAgentInvocation`. Both belong under `params:`, NOT at the top of the step.

**Wrong:**

```yaml
- id: think
  type: agent
  agent: planner
  instruction: "outline a plan"   # ignored — goes into step.params by accident
  ephemeral: true                 # ignored
```

**Right:**

```yaml
- id: think
  type: agent
  agent: planner
  params:
    instruction: "outline a plan"
    ephemeral: true
```

## Parallel step symbols are snapshotted per task

**Change:** Slice 6d (`acfd6b6`) made parallel tasks take a `const` copy of the symbols map. Concurrent reads are safe; concurrent writes are not (and not supported).

**Defensive authoring:** Don't try to make parallel tasks mutate shared state. Pass inputs via `${input.*}` and read outputs from `result.outputs[id]` after the parallel block returns.

## Per-turn streaming state reset

**Change:** Slice `a6d2731` reset per-turn streaming state at the start of every prompt. If you observe hard blocks on the second query, you're probably running an old build.

**Defensive authoring:** Always pull + rebuild before live-testing. The reset block is at the top of `Agent::processUserMessage` (or similar — read the source).

## Sub-agent prompts use minimal metadata

**Sub-agent `<sub_agent>` blocks** expose only: name, version, summary, provider, model, manifest_dir, and child tool names/descriptions. NOT the sub-agent's full system prompt. This is by design — the parent shouldn't see the child's full prompt. If you need to share prompts, use a shared persona file.

## Audit-driven schema additions

The audit (`docs/TICKETS.md`) and `agent-lib-cpp-poc-stub-audit-2026-06-20` track schema changes. Before authoring, read the latest "Done" items in `docs/TICKETS.md` to know which fields are recognized.

## What the parser does NOT do

- No JSON-Schema validation. Param types are not enforced.
- No cross-reference validation. A `tool: nonexistent` is accepted and only fails at runtime.
- No depth limits on recursive workflows.
- No uniqueness check on step IDs.
- No UTF-8 validation. Bad UTF-8 in a value may produce mojibake later.