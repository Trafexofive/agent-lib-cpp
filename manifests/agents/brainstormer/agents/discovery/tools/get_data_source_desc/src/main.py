#!/usr/bin/env python3
"""get_data_source_desc — describe available structured data sources."""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, "..", "..", "_shared", "src")))

from common import read_params, emit, fail  # noqa: E402
from data_sources import load_catalog, describe_source  # noqa: E402


def main() -> int:
    params = read_params()
    source_id = (params.get("source_id") or "").strip()
    category = (params.get("category") or "").strip().lower()

    if source_id:
        src = describe_source(source_id)
        if src is None:
            fail(f"unknown source '{source_id}'")
        emit({"success": True, "count": 1, "sources": [src]})
        return 0

    sources = load_catalog()
    if category:
        sources = [s for s in sources if s.get("category") == category]

    emit({"success": True, "count": len(sources), "sources": sources})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
