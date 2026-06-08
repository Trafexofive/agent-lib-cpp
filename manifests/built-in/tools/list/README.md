# list

**Type:** tool  
**Runtime:** builtin (C++ — `internal_tools.cpp`)  
**Status:** ✅ stable

## Purpose
Lists directory contents. Supports recursive listing, glob filtering, and detail levels (names, compact, full).

## Schema
See `tool.yml` for input/output schemas.

## Usage
```yaml
import:
  tools:
    - list
```

Agent calls:
```xml
<action type="tool" name="list" id="ls1" mode="sync">
  {"path": ".", "recursive": false, "detail": "compact"}
</action>
```

## Variants
- `recursive: true` — full tree
- `pattern: "*.cpp"` — glob filter
- `detail: "full"` — sizes, permissions, timestamps

## Dependencies
- POSIX `opendir`/`readdir`/`stat` (stdlib)
- No external runtime needed
