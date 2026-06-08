# fs_read

**Type:** tool  
**Runtime:** python3  
**Status:** ✅ stable

## Purpose
Reads file contents with offset/limit support. Supports all file types — text and binary (binary output base64-encoded).

## Schema
See `tool.yml` for input/output schemas.

## Usage
```yaml
import:
  tools:
    - ./tools/fs_read/tool.yml
```

Agent calls:
```xml
<action type="tool" name="fs_read" id="r1" mode="sync">
  {"path": "src/core/agent.hpp", "offset": 0, "limit": 50}
</action>
```

## Dependencies
- Python 3 (standard library only)
- File must be within workspace or explicitly allowed paths

## Edge Cases
- File not found → `{"success": false, "error": "..."}`
- Binary file → content base64-encoded, `binary: true`
- Large files → truncated at `limit` lines; `truncated: true` in response
