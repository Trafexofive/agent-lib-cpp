# Staged Feeds

Feeds in this directory are under active development and testing.

## Lifecycle
```
config/staging/feeds/    ← You are here (create, test, iterate)
        │
        ▼  (when stable & tested)
manifests/feeds/         ← Promoted to global scope
```

## Feed Manifest Schema

```yaml
kind: Feed
name: <unique_name>           # Used as <feed_name> XML tag
version: "1.0"
summary: "What this feed provides"
runtime: python3 | bash | node | builtin
entrypoint: ./feed.py         # Script to execute (relative to manifest)
interval_secs: 60             # Poll interval (for scheduler)
output_schema:                # JSON Schema for feed output
  type: object
  properties:
    key:
      type: string
      description: "..."
```

## Available Runtimes

| Runtime | Executor | Best for |
|---------|----------|----------|
| `builtin` | C++ (feed_engine.hpp) | System-level feeds (clock, stats) |
| `python3` | Python 3 interpreter | Data processing, API calls |
| `bash` / `sh` | Shell | Quick CLI-based feeds |
| `node` | Node.js | JS ecosystem feeds |

## Current Feeds

| Feed | Runtime | Status | Description |
|------|---------|--------|-------------|
| — | — | — | Staging is for WIP modules. |

Built-in feeds live at `manifests/built-in/feeds/`. Example feeds live at `examples/feeds/`.
