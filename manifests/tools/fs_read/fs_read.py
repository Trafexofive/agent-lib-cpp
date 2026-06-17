#!/usr/bin/env python3
"""fs_read — Read file contents with offset/limit support."""
import json
import sys

def load_params():
    if len(sys.argv) <= 1:
        return {}
    arg = sys.argv[1]
    try:
        with open(arg, "r", encoding="utf-8") as f:
            return json.loads(f.read())
    except OSError:
        pass
    return json.loads(arg)

def main():
    try:
        params = load_params()
    except (json.JSONDecodeError, ValueError) as e:
        print(json.dumps({"error": f"invalid JSON input: {e}"}))
        sys.exit(1)
    path = params.get("path", "")
    offset = int(params.get("offset", 0))
    limit = int(params.get("limit", 0))

    if not path:
        print(json.dumps({"error": "path is required"}))
        sys.exit(1)

    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(json.dumps({"error": f"file not found: {path}"}))
        sys.exit(1)
    except PermissionError:
        print(json.dumps({"error": f"permission denied: {path}"}))
        sys.exit(1)

    total = len(lines)
    selected = lines[offset:] if limit == 0 else lines[offset:offset + limit]
    content = "".join(selected)
    truncated = limit > 0 and total > offset + limit

    print(json.dumps({
        "content": content,
        "lines": total,
        "truncated": truncated
    }))

if __name__ == "__main__":
    main()
