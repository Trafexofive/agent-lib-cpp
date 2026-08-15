#!/usr/bin/env python3
"""web_search — read-only web search (SearXNG when configured, else DDG)."""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, "..", "..", "_shared", "src")))

from common import read_params, emit, fail, web_search  # noqa: E402


def main() -> int:
    params = read_params()
    query = (params.get("query") or "").strip()
    if not query:
        fail("query is required")

    try:
        num = int(params.get("num_results", 10))
    except (TypeError, ValueError):
        num = 10
    num = max(1, min(num, 30))

    try:
        out = web_search(query, num)
    except Exception as e:
        fail(str(e))

    out["success"] = True
    out["query"] = query
    emit(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
