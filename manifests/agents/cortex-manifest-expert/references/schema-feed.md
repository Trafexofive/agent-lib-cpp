# Feed Manifest Schema

**Authored at:** `config/agents/<agent>/<feed-name>/feed.yml` (agent-local) or `manifests/feeds/<name>/feed.yml`

**Parsed by:** `src/feeds/feed_engine.hpp::loadFeedManifest`

**Loaded by:** `src/core/manifest_loader.hpp::loadFeeds` — paths from `import.feeds:` list, relative to the manifest's directory.

## Required fields

```yaml
kind: Feed
name: <snake_case>                # unique feed name; the model addresses it as the feed
version: "<semver>"
summary: "<one-line, present-tense>"
```

## Behavior: poll runtime

The feed has a poll function that's invoked at the start of each turn to produce ambient context.

```yaml
runtime: python3                  # python3, bash, node, process, binary, exec, direct
entrypoint: ./poll.py             # script that emits a JSON document to stdout
```

The poll script reads stdin/env, does its work, and writes JSON to stdout. The output becomes the `<feeds>` block in the prompt.

If `runtime: builtin`, the manifest still registers a Feed (so manifest-declared tools can be wired onto it) but the poll is a no-op.

## Optional: per-tool runtime + entrypoint (manifest-declared tools)

A feed can expose callable tools to reconfigure its resource (e.g., a `status` tool that reads state, a `set` tool that writes state). Each tool runs as a separate process invocation with `FEED_TOOL_PARAMS` set to the action's JSON params.

```yaml
tools:
  - name: status
    description: Read the current state
    runtime: python3
    entrypoint: ./tool_status.py
    build:                          # optional per-tool build
      command: "make"
      cwd: "./"
      output: "./bin/tool"
  - name: set
    description: Write a key/value into state
    runtime: python3
    entrypoint: ./tool_set.py
```

**Important:** The model can only invoke a tool if it has a runtime handler. Until you wire the manifest tools onto the feed (this happens automatically in `loadFeedManifest`), the tool is NOT advertised in `<action_available>`. This prevents the model from seeing tools it can't call.

When loaded, manifest tools are wired through `process::run` with a 30s timeout and 1MB stdout cap.

## Optional: allow_empty

Default behavior: a feed poll that returns no stdout is a **load failure**. The error mentions `allow_empty` so authors know the opt-in.

```yaml
allow_empty: true                 # opt in if the feed legitimately reports nothing
```

Use sparingly. Empty output usually means a real bug.

## Invocation syntax

```xml
<!-- poll (ambient context) -->
<action type="feed" name="<feed>" id="f1" mode="sync">{}</action>

<!-- call a tool -->
<action type="feed" name="<feed>.<tool>" id="f2" mode="sync">{"key":"value"}</action>
```

The dotted `<feed>.<tool>` form is **always** a tool call. Unknown dotted names hard-error (no silent fallback to a literal feed name).

## Inputs to the poll script

None by default. The poll script runs in a fresh process; it can read env, files, network — whatever it needs.

## Inputs to a tool script

`FEED_TOOL_PARAMS` is set to the action's JSON body (e.g., `{"key":"value"}`). `CALL_TOOL` is set to the path of the `call-tool` helper, in case the script wants to invoke other tools.

## Output contract

The script's stdout is parsed as JSON. If parsing succeeds, the parsed object becomes the tool's result. Otherwise, the raw text goes into `output` and `success` is derived from the exit code.

## Examples

- `config/agents/morpheus/morpheus_dashboard/feed.yml` — 4-tool feed with state file + log
- `examples/feeds/example-bash/feed.yml` — bash feed
- `examples/feeds/example-python/feed.yml` — Python feed

## Common mistakes

1. **Empty stdout without `allow_empty: true`** — load fails with "feed script returned empty output (set allow_empty: true to allow)".
2. **First `name:` of a tool entry not being picked up** — block-style `- name: foo` has a parser quirk where `ManifestYaml::get(item, "name")` returns "" unless the entry has a child `name:` key. The feed parser has a fallback for `entry.key == "name"`, but other list-of-maps parsers (e.g., tool `examples:`) may not. Always use either an explicit child `name:` key or test it.
3. **`runtime:` not matching the entrypoint language** — `runtime: bash` + a Python script will fail with a Python syntax error from `bash`.
4. **Tool `entrypoint:` as an absolute path** — paths are relative to the manifest directory. Use `./tool.py`.
5. **Forgetting to add the feed to `import.feeds` in the agent manifest** — the manifest parses, but the feed never loads.