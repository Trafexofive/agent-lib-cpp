# Broken: feed path missing `./` prefix

## The broken manifest

```yaml
kind: Agent
name: my_agent
version: "1.0"
summary: "Imports a feed with a relative path but no ./ prefix."

import:
  feeds:
    - my_dashboard/feed.yml      # ← bug
```

## What goes wrong

`isPathImport("my_dashboard/feed.yml")` returns `false` because:
- It doesn't end in `.yml` (well, it does end in `.yml`, so it should... wait, it does. Let me re-check.)

Actually: `my_dashboard/feed.yml` DOES end in `.yml`, so `isPathImport` returns true. The path resolves to `parent_path() / "my_dashboard/feed.yml"`. That works.

The actual bug case: `my_dashboard` (no `.yml` extension) is treated as a builtin feed name. The agent tries to register a feed named `my_dashboard` via `agent.addFeed(stripBuiltinPrefix("my_dashboard"))` and fails silently.

```yaml
import:
  feeds:
    - my_dashboard       # ← bug: treated as a name, not a path
```

## What goes wrong (the actual symptom)

The feed manifest never gets loaded. The model has no `<my_dashboard>` in its prompt. If `my_dashboard` isn't a registered builtin feed name, the dispatch is a no-op.

## The fix

Always use `./` (or `/` or `.yml`) for relative paths:

```yaml
import:
  feeds:
    - ./my_dashboard/feed.yml   # explicit path
```

## Detection

Look at the agent's startup log. If the feed isn't in the loaded manifest list, the path resolution went wrong. Cross-check with `git ls-files manifests/feeds/` and the file system to confirm where the feed actually lives.

## Tests

`src/core/manifest_loader.hpp::isPathImport` (line 57) has the heuristic. Read it to understand exactly what counts as a path.