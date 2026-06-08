#!/usr/bin/env python3
"""json_tool — Parse, query, and transform JSON data."""
import json
import sys
import re

def resolve_query(data, query):
    """Resolve a jq-style dot-notation query against a dict/list."""
    if not query or query == ".":
        return data
    # Strip leading dot
    q = query.lstrip(".")
    parts = re.split(r'\.(?![^\[]*\])', q)
    result = data
    for part in parts:
        if not part:
            continue
        # Handle array indexing: key[0] or [0]
        match = re.match(r'(\w*)\[(\d+)\]', part)
        if match:
            key, idx = match.group(1), int(match.group(2))
            if key:
                result = result[key]
            result = result[idx]
        elif isinstance(result, dict):
            result = result.get(part)
        elif isinstance(result, list):
            # Try int index
            try:
                result = result[int(part)]
            except (ValueError, IndexError):
                return None
        else:
            return None
        if result is None:
            return None
    return result

def main():
    try:
        params = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    except (json.JSONDecodeError, ValueError) as e:
        print(json.dumps({"error": f"invalid JSON input: {e}"}))
        sys.exit(1)
    op = params.get("op", "parse")
    data_str = params.get("data", "")
    file_path = params.get("path", "")
    query = params.get("query", "")
    keys = params.get("keys", [])

    # Load JSON
    raw = ""
    if file_path:
        try:
            with open(file_path, "r") as f:
                raw = f.read()
        except (FileNotFoundError, PermissionError) as e:
            print(json.dumps({"error": str(e)}))
            sys.exit(1)
    else:
        raw = data_str

    if op in ("validate", "parse", "pretty", "minify"):
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as e:
            print(json.dumps({"valid": False, "error": str(e)}))
            sys.exit(1)

        if op == "validate":
            print(json.dumps({"valid": True}))
        elif op == "parse":
            print(json.dumps(parsed, indent=2))
        elif op == "pretty":
            print(json.dumps(parsed, indent=2))
        elif op == "minify":
            print(json.dumps(parsed, separators=(",", ":")))

    elif op == "query":
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as e:
            print(json.dumps({"error": f"invalid JSON: {e}"}))
            sys.exit(1)
        result = resolve_query(parsed, query)
        if result is None:
            print(json.dumps({"error": f"query returned null: {query}"}))
        else:
            print(json.dumps(result, indent=2))

    elif op == "extract":
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as e:
            print(json.dumps({"error": f"invalid JSON: {e}"}))
            sys.exit(1)
        result = {}
        for k in keys:
            result[k] = resolve_query(parsed, k)
        print(json.dumps(result, indent=2))

    else:
        print(json.dumps({"error": f"unknown op: {op}"}))
        sys.exit(1)

if __name__ == "__main__":
    main()
