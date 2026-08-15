#!/usr/bin/env python3
"""search_image_by_image — reverse image lookup via local fingerprinting."""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, "..", "..", "_shared", "src")))

from common import (  # noqa: E402
    read_params, emit, fail, image_metadata, load_image_bytes, find_similar,
    searxng_image_search,
)


def main() -> int:
    params = read_params()
    image = (params.get("image") or "").strip()
    if not image:
        fail("image is required (URL or local path)")

    try:
        max_hamming = int(params.get("max_hamming", 10))
    except (TypeError, ValueError):
        max_hamming = 10
    max_hamming = max(0, min(max_hamming, 64))
    try:
        limit = int(params.get("limit", 10))
    except (TypeError, ValueError):
        limit = 10
    limit = max(1, min(limit, 50))

    try:
        data, label = load_image_bytes(image)
    except Exception as e:
        fail(f"could not load image: {e}")

    try:
        fp = image_metadata(data)
    except Exception as e:
        fail(f"could not fingerprint image: {e}")

    out = {"success": True, "fingerprint": fp, "source": label}

    corpus = (params.get("corpus_dir") or "").strip()
    if corpus:
        out["near_duplicates"] = find_similar(corpus, fp["dhash"], max_hamming,
                                              limit)

    # Remote reverse-image via SearXNG when configured (best-effort).
    remote = searxng_image_search(f"image_url={label}", limit) if image.startswith("http") else []
    if remote and "error" not in remote[0]:
        out["remote"] = remote

    emit(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
