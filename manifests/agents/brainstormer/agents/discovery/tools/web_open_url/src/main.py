#!/usr/bin/env python3
"""web_open_url — fetch a URL and extract readable content."""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, "..", "..", "_shared", "src")))

from common import read_params, emit, fail, fetch_readable  # noqa: E402


def main() -> int:
    params = read_params()
    url = (params.get("url") or "").strip()
    if not url:
        fail("url is required")
    if not url.startswith(("http://", "https://")):
        fail(f"unsupported scheme (http/https only): {url}")

    try:
        max_chars = int(params.get("max_chars", 30000))
    except (TypeError, ValueError):
        max_chars = 30000
    max_chars = max(1, min(max_chars, 200000))

    try:
        out = fetch_readable(url, max_chars)
    except Exception as e:
        fail(str(e))

    emit(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
