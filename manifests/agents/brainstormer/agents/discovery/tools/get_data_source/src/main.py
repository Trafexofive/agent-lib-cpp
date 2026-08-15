#!/usr/bin/env python3
"""get_data_source — query a structured data source by id."""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, "..", "..", "_shared", "src")))

from common import read_params, emit, fail  # noqa: E402
from data_sources import query_source  # noqa: E402


def main() -> int:
    params = read_params()
    source = (params.get("source") or "").strip().lower()
    if not source:
        fail("source is required")

    query = (params.get("query") or "").strip()
    mode = (params.get("mode") or "search").strip().lower()
    try:
        max_results = int(params.get("max_results", 10))
    except (TypeError, ValueError):
        max_results = 10
    max_results = max(1, min(max_results, 200))

    # Normalise params per adapter signature.
    adapter_params: dict = {}
    if source == "arxiv":
        if not query:
            fail("query is required for arxiv")
        adapter_params = {"query": query, "max_results": max_results}
    elif source == "crossref":
        if not query:
            fail("query is required for crossref")
        adapter_params = {"query": query, "rows": max_results}
    elif source == "openalex":
        if not query:
            fail("query is required for openalex")
        adapter_params = {"query": query, "per_page": max_results}
    elif source == "wikipedia":
        if not query:
            fail("query is required for wikipedia")
        adapter_params = {"query": query, "mode": mode, "limit": max_results}
    elif source == "yahoo":
        if not query:
            fail("query (symbol) is required for yahoo")
        adapter_params = {"symbol": query}
    elif source == "github":
        if not query:
            fail("query is required for github")
        adapter_params = {"query": query, "per_page": max_results}
    else:
        fail(f"unknown source '{source}'")

    try:
        out = query_source(source, adapter_params)
    except Exception as e:
        fail(str(e))

    emit(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
