#!/usr/bin/env python3
"""fs_write — Write content to a file."""
import json
import sys
import os

def main():
    try:
        params = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    except (json.JSONDecodeError, ValueError) as e:
        print(json.dumps({"error": f"invalid JSON input: {e}"}))
        sys.exit(1)
    path = params.get("path", "")
    content = params.get("content", "")
    append = params.get("append", False)

    if not path:
        print(json.dumps({"error": "path is required"}))
        sys.exit(1)

    # Create parent directories
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)

    try:
        mode = "a" if append else "w"
        with open(path, mode) as f:
            written = f.write(content)
    except PermissionError:
        print(json.dumps({"error": f"permission denied: {path}"}))
        sys.exit(1)
    except OSError as e:
        print(json.dumps({"error": str(e)}))
        sys.exit(1)

    print(json.dumps({
        "success": True,
        "path": path,
        "bytes_written": written
    }))

if __name__ == "__main__":
    main()
