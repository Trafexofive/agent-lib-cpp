# Tool Manifest Schema

**Authored at:** `manifests/built-in/tools/<name>/tool.yml` (or `config/agents/<agent>/<name>/tool.yml` for agent-local tools)

**Parsed by:** `src/tools/tool.hpp` (see `ToolDef`), `src/core/manifest_autoload.hpp` (recursive loader)

**Loaded by:** `src/core/manifest_autoload.hpp::loadToolsForAgent` — auto-discovers from `manifests/built-in/tools/<name>/` directories.

## Required fields

```yaml
kind: Tool                        # discriminator; must be exactly "Tool"
name: <snake_case>                # exact name; what you pass to <action name="...">
version: "<semver>"               # e.g., "1.0", "2.3.1"
summary: "<one-line description>" # shown in <action_available> listings
```

## Common fields

```yaml
description: |                    # multi-line, shown in <action_available>
  Long form of what the tool does.
author: "<who>"                   # optional, no functional effect
tags:                             # optional, free-form labels
  - math
  - finance
```

## Behavior: native vs script

A tool is either a C++-registered function (built-in like `fs_read`, `exec`) or a script-backed tool. For script tools:

```yaml
runtime: python3                  # python3, bash, node, process, binary, exec, direct
                                  # process/binary/exec/direct run the entrypoint directly
entrypoint: ./main.py             # relative to the manifest directory
```

If `entrypoint` is set, the loader creates a `Tool` that runs the entrypoint as a script. Otherwise the tool is treated as native and looked up in the C++ registry.

## Optional: build hook

If the entrypoint is a compiled binary, declare a build step:

```yaml
build:
  command: "make"                 # shell command to build
  cwd: "./"                       # working dir for the build
  output: "./bin/mybinary"        # if present, build is skipped
```

Build runs lazily on first invocation. `output` is the "already built" check.

## Optional: input contract

For script tools, declare how params are passed:

```yaml
input_type: "json"                # default; the whole params object is written to a temp file
input_param: "text"               # if input_type is "text", this is the param name passed as argv
input_text_param: "prompt"        # legacy alias
```

## Optional: schema

JSON-schema-style description of expected params. Surfaced in `<action_available>` as the `params` block.

```yaml
params:
  properties:
    path:
      type: string
      required: true
    limit:
      type: integer
      default: 100
  required: [path]
```

## Optional: result contract

```yaml
returns: "<description of return value>"
returns_schema:
  type: object
  properties: { ... }
```

These are surfaced as `<returns>` and `<examples>` blocks when `prompt_building.runtime_capabilities` is enabled.

## Optional: response contract

```yaml
is_mutating: true                 # hint: this tool changes state, dedup cache invalidates
is_async: false                   # hint: not yet implemented
```

## Examples

- `manifests/built-in/tools/exec/tool.yml` — native tool, no entrypoint
- `manifests/built-in/tools/grep/tool.yml` — native tool, no entrypoint
- `examples/runtime-templates/c-tool/tool.yml` — compiled C tool with build hook
- `examples/runtime-templates/cpp-tool/tool.yml` — compiled C++ tool with build hook
- `playground/diagram-junky/manifests/tools/diagram-render/tool.yml` — Python script tool

## Common mistakes

1. **Forgetting `kind: Tool`** — the autoloader keys on this; without it, the manifest is silently ignored.
2. **Top-level `name:` not snake_case** — the dispatcher normalizes names, but the action `<action name="...">` is matched on the normalized form.
3. **Mismatched `runtime` / `entrypoint`** — `runtime: bash` + `entrypoint: main.py` will run `bash main.py` (which fails). Use `runtime: python3` for Python scripts.
4. **`build.output` pointing to a non-existent path** — the build runs on first invocation. If the path is wrong, you'll get a confusing build error in production.
5. **Not setting `is_mutating: true` for write tools** — the dedup cache returns stale results across calls. See `src/core/agent_tools.cpp:dispatch` for the dedup key.