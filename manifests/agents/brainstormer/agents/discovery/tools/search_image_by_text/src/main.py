#!/usr/bin/env python3
"""search_image_by_text — find images by concept/query."""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, "..", "..", "_shared", "src")))

from common import read_params, emit, fail, image_search  # noqa: E402


def main() -> int:
    params = read_params()
    query = (params.get("query") or "").strip()
    if not query:
        fail("query is required")

    try:
        limit = int(params.get("limit", 10))
    except (TypeError, ValueError):
        limit = 10
    limit = max(1, min(limit, 30))

    try:
        out = image_search(query, limit)
    except Exception as e:
        fail(str(e))

    out["success"] = True
    out["query"] = query
    emit(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
