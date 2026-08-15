"""Shared, side-effect-free helpers for discovery tools.

Every discovery tool is read-only. This module centralises the plumbing that
would otherwise be copy-pasted into each tool script:

  * argv[1] JSON parameter decoding (the tool runtime passes a temp file path)
  * single-JSON-document stdout emission (the tool contract)
  * bounded HTTP GET (requests, with clean error surfaces)
  * web search backends (SearXNG when configured, DuckDuckGo HTML otherwise)
  * readable-content extraction (trafilatura, bs4 fallback)
  * image search (Wikimedia Commons, optional SearXNG images)
  * local image fingerprinting (dHash + hamming distance) for reverse search

No module writes to disk, spawns side-effecting subprocesses, or touches state
beyond the read it was asked to perform.
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Any, Optional
from urllib.parse import quote_plus, urljoin

import requests

from PIL import Image, ImageFile

# PIL 10+ raises on truncated images; for recon we prefer a best-effort
# fingerprint over a hard failure on a slightly-cut image.
ImageFile.LOAD_TRUNCATED_IMAGES = True

# --------------------------------------------------------------------------
# Environment knobs (self-hosted first, no SaaS dependency by default)
# --------------------------------------------------------------------------
SEARXNG_URL = os.environ.get("SEARXNG_URL", "").rstrip("/")
UA = os.environ.get("DISCOVERY_USER_AGENT", "cortex-discovery/1.0 (read-only recon)")

REQUEST_TIMEOUT = float(os.environ.get("DISCOVERY_TIMEOUT", "30"))
MAX_BODY_BYTES = int(os.environ.get("DISCOVERY_MAX_BODY", str(2 * 1024 * 1024)))
IMAGE_MAX_BYTES = int(os.environ.get("DISCOVERY_MAX_IMAGE", str(25 * 1024 * 1024)))


# --------------------------------------------------------------------------
# Parameter + output plumbing
# --------------------------------------------------------------------------
def read_params() -> dict[str, Any]:
    """Decode tool params from argv[1].

    The Cortex tool runtime passes either a path to a JSON temp file (json
    input type) or the raw text (text input type). We accept both.
    """
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        return {}
    arg = sys.argv[1]
    # Path-looking argument -> read the file.
    if arg.startswith(("/", "./", "../")) or os.path.exists(arg):
        try:
            return json.loads(Path(arg).read_text())
        except (OSError, json.JSONDecodeError):
            pass
    try:
        return json.loads(arg)
    except json.JSONDecodeError:
        return {}


def emit(result: dict[str, Any], exit_code: int = 0) -> "NoReturn":
    print(json.dumps(result, ensure_ascii=False))
    raise SystemExit(exit_code)


def fail(message: str, **extra: Any) -> "NoReturn":
    emit({"success": False, "error": message, **extra}, 1)


# --------------------------------------------------------------------------
# Bounded HTTP
# --------------------------------------------------------------------------
class HttpError(Exception):
    def __init__(self, status: int, detail: str):
        self.status = status
        self.detail = detail
        super().__init__(f"HTTP {status}: {detail}")


def http_get(url: str, *, params: Optional[dict] = None,
             timeout: float = REQUEST_TIMEOUT,
             headers: Optional[dict] = None,
             max_bytes: int = MAX_BODY_BYTES) -> tuple[int, bytes, str, str]:
    """GET a URL, returning (status, body_bytes, final_url, content_type).

    Raises HttpError on transport failure or non-2xx. Bounds the response to
    max_bytes and follows redirects (final_url reports the landing page).
    """
    hdrs = {"User-Agent": UA, "Accept": "*/*"}
    if headers:
        hdrs.update(headers)
    try:
        resp = requests.get(url, params=params, headers=hdrs, timeout=timeout,
                            stream=True, allow_redirects=True)
    except requests.RequestException as e:
        raise HttpError(0, f"request failed: {e}") from e

    final_url = resp.url
    content_type = resp.headers.get("Content-Type", "")
    try:
        resp.raise_for_status()
    except requests.HTTPError as e:
        raise HttpError(resp.status_code, f"{e}") from e

    body = b""
    for chunk in resp.iter_content(chunk_size=65536):
        if not chunk:
            continue
        if len(body) + len(chunk) > max_bytes:
            body += chunk[: max_bytes - len(body)]
            break
        body += chunk
    return resp.status_code, body, final_url, content_type


def http_get_text(url: str, **kw: Any) -> tuple[int, str, str, str]:
    status, body, final_url, ctype = http_get(url, **kw)
    return status, body.decode("utf-8", errors="replace"), final_url, ctype


# --------------------------------------------------------------------------
# Web search
# --------------------------------------------------------------------------
def searxng_search(query: str, num_results: int = 10,
                   categories: str = "general") -> dict[str, Any]:
    """Query a self-hosted SearXNG instance's JSON API."""
    if not SEARXNG_URL:
        return {"available": False, "reason": "SEARXNG_URL not set"}
    try:
        status, text, _final, _ct = http_get_text(
            SEARXNG_URL + "/search",
            params={"q": query, "format": "json", "categories": categories},
            headers={"Accept": "application/json"},
        )
        data = json.loads(text)
    except (HttpError, json.JSONDecodeError) as e:
        return {"available": False, "reason": str(e), "error": str(e)}

    results = []
    for r in data.get("results", [])[:num_results]:
        results.append({
            "title": r.get("title", ""),
            "url": r.get("url", ""),
            "content": r.get("content", "") or r.get("snippet", ""),
            "engine": r.get("engine", ""),
        })
    return {"available": True, "results": results, "count": len(results)}


# DDG lite HTML result shapes.
_DDG_RESULT_RE = re.compile(
    r'<a[^>]+class="result__a"[^>]+href="([^"]+)"[^>]*>(.*?)</a>', re.DOTALL)
_DDG_SNIPPET_RE = re.compile(
    r'<a[^>]+class="result__snippet"[^>]*>(.*?)</a>', re.DOTALL)
_TAG_RE = re.compile(r"<[^>]+>")

_clean = lambda s: re.sub(r"\s+", " ", _TAG_RE.sub("", s)).strip()  # noqa: E731


def ddg_search(query: str, num_results: int = 10) -> dict[str, Any]:
    """Scrape DuckDuckGo's HTML (lite) endpoint. No key, no service."""
    url = "https://html.duckduckgo.com/html/"
    try:
        _status, page, _final, _ct = http_get_text(
            url, params={"q": query},
            headers={"User-Agent": "Mozilla/5.0 (X11; Linux x86_64)"})
    except HttpError as e:
        return {"available": False, "reason": str(e), "results": []}

    titles = _DDG_RESULT_RE.findall(page)
    snippets = _DDG_SNIPPET_RE.findall(page)

    results = []
    for i, (href, title) in enumerate(titles):
        if i >= num_results:
            break
        real_url = href
        if "uddg=" in href:
            real_url = re.sub(r"^.*uddg=", "", href).split("&", 1)[0]
            try:
                from urllib.parse import unquote
                real_url = unquote(real_url)
            except Exception:
                pass
        snippet = _clean(snippets[i][0]) if i < len(snippets) else ""
        results.append({
            "title": _clean(title),
            "url": real_url,
            "snippet": snippet,
        })
    return {"available": bool(results), "results": results, "count": len(results)}


def web_search(query: str, num_results: int = 10) -> dict[str, Any]:
    """Search the web, preferring the self-hosted SearXNG when present."""
    if SEARXNG_URL:
        out = searxng_search(query, num_results)
        if out.get("available"):
            return {"backend": "searxng", **out}
    out = ddg_search(query, num_results)
    return {"backend": "duckduckgo", **out}


# --------------------------------------------------------------------------
# Readable content extraction
# --------------------------------------------------------------------------
def extract_readable(html: str, url: str = "") -> str:
    """Best-effort readable text from raw HTML."""
    try:
        import trafilatura
        text = trafilatura.extract(html, include_comments=False,
                                   include_tables=True)
        if text and text.strip():
            return text.strip()
    except Exception:
        pass
    # bs4 fallback: strip script/style/nav/footer/header, then tags.
    try:
        from bs4 import BeautifulSoup
        soup = BeautifulSoup(html, "lxml")
        for tag in soup(["script", "style", "nav", "footer", "header",
                         "aside", "form", "noscript"]):
            tag.decompose()
        return re.sub(r"\n{3,}", "\n\n", soup.get_text(" ", strip=True))
    except Exception:
        return re.sub(r"\s+", " ", _TAG_RE.sub(" ", html)).strip()


def fetch_readable(url: str, max_chars: int = 30000) -> dict[str, Any]:
    """Fetch a URL and return title + readable text + metadata."""
    try:
        status, html, final_url, ctype = http_get_text(url)
    except HttpError as e:
        return {"success": False, "error": str(e), "status": e.status,
                "url": url}

    title = ""
    m = re.search(r"<title[^>]*>(.*?)</title>", html, re.DOTALL | re.I)
    if m:
        title = _clean(m.group(1))

    text = extract_readable(html, final_url)
    truncated = len(text) > max_chars
    if truncated:
        text = text[:max_chars]

    return {
        "success": True,
        "title": title,
        "url": final_url,
        "final_url": final_url,
        "content_type": ctype,
        "status": status,
        "content": text,
        "length": len(text),
        "truncated": truncated,
    }


# --------------------------------------------------------------------------
# Image search (by text)
# --------------------------------------------------------------------------
def wikimedia_image_search(query: str, limit: int = 10) -> list[dict[str, Any]]:
    """Search Wikimedia Commons for images matching a text query."""
    api = "https://commons.wikimedia.org/w/api.php"
    params = {
        "action": "query",
        "generator": "search",
        "gsrsearch": query,
        "gsrlimit": str(limit),
        "gsrnamespace": "6",  # File namespace
        "prop": "imageinfo",
        "iiprop": "url|size|mime|extmetadata",
        "iiurlwidth": "400",
        "format": "json",
    }
    try:
        _status, text, _final, _ct = http_get_text(api, params=params)
        data = json.loads(text)
    except (HttpError, json.JSONDecodeError) as e:
        return [{"error": str(e)}]

    out: list[dict[str, Any]] = []
    for page in data.get("query", {}).get("pages", {}).values():
        info = (page.get("imageinfo") or [{}])[0]
        meta = info.get("extmetadata") or {}
        lic = meta.get("LicenseShortName", {}).get("value", "")
        artist = meta.get("Artist", {})
        artist = artist.get("value", "") if isinstance(artist, dict) else ""
        out.append({
            "title": page.get("title", ""),
            "url": info.get("url", ""),
            "thumb_url": info.get("thumburl", info.get("url", "")),
            "width": info.get("width"),
            "height": info.get("height"),
            "mime": info.get("mime", ""),
            "description_url": info.get("descriptionurl", ""),
            "license": lic,
            "artist": _clean(artist)[:200],
        })
    return out


def searxng_image_search(query: str, limit: int = 10) -> list[dict[str, Any]]:
    if not SEARXNG_URL:
        return [{"error": "SEARXNG_URL not set"}]
    try:
        _status, text, _final, _ct = http_get_text(
            SEARXNG_URL + "/search",
            params={"q": query, "format": "json", "categories": "images"},
            headers={"Accept": "application/json"},
        )
        data = json.loads(text)
    except (HttpError, json.JSONDecodeError) as e:
        return [{"error": str(e)}]
    out = []
    for r in data.get("results", [])[:limit]:
        out.append({
            "title": r.get("title", ""),
            "url": r.get("url", ""),
            "thumb_url": r.get("thumbnail_src", r.get("img_src", "")),
            "source": r.get("source", ""),
            "engine": r.get("engine", ""),
        })
    return out


def image_search(query: str, limit: int = 10) -> dict[str, Any]:
    """Image search by text, preferring SearXNG images when configured."""
    if SEARXNG_URL:
        results = searxng_image_search(query, limit)
        if results and "error" not in results[0]:
            return {"backend": "searxng", "results": results,
                    "count": len(results)}
    results = wikimedia_image_search(query, limit)
    return {"backend": "wikimedia-commons", "results": results,
            "count": len(results)}


# --------------------------------------------------------------------------
# Local image fingerprinting (reverse image search)
# --------------------------------------------------------------------------
def _load_image(data: bytes):
    import io
    return Image.open(io.BytesIO(data))


def dhash(data: bytes, hash_size: int = 8) -> str:
    """Difference hash (64-bit hex) — robust to resizing/light transforms."""
    img = _load_image(data).convert("L")
    img = img.resize((hash_size + 1, hash_size), Image.Resampling.LANCZOS)
    px = list(img.getdata())
    bits = 0
    for row in range(hash_size):
        for col in range(hash_size):
            left = px[row * (hash_size + 1) + col]
            right = px[row * (hash_size + 1) + col + 1]
            bits = (bits << 1) | (1 if left > right else 0)
    return f"{bits:0{hash_size * hash_size}x}"


def hamming_distance(h1: str, h2: str) -> int:
    a, b = int(h1, 16), int(h2, 16)
    return bin(a ^ b).count("1")


def image_metadata(data: bytes) -> dict[str, Any]:
    """Dimensions, format, mode, dominant color, and dHash fingerprint."""
    img = _load_image(data)
    md: dict[str, Any] = {
        "width": img.width,
        "height": img.height,
        "format": img.format,
        "mode": img.mode,
        "dhash": dhash(data),
    }
    try:
        small = img.convert("RGB").resize((1, 1), Image.Resampling.BOX)
        r, g, b = small.getpixel((0, 0))
        md["dominant_color"] = f"#{r:02x}{g:02x}{b:02x}"
    except Exception:
        pass
    return md


def load_image_bytes(source: str) -> tuple[bytes, str]:
    """Load image bytes from a URL or local path, returning (bytes, label)."""
    if re.match(r"^https?://", source):
        status, body, final_url, ctype = http_get(source, max_bytes=IMAGE_MAX_BYTES)
        if not body:
            raise HttpError(status, "empty image response")
        return body, final_url
    p = Path(source).expanduser()
    if not p.is_file():
        raise FileNotFoundError(f"no such file: {source}")
    return p.read_bytes(), str(p.resolve())


def find_similar(corpus_dir: str, target_hash: str,
                 max_hamming: int = 10, limit: int = 10) -> list[dict[str, Any]]:
    """Scan a local image corpus for near-duplicates of target_hash."""
    root = Path(corpus_dir).expanduser()
    if not root.is_dir():
        return [{"error": f"corpus directory not found: {corpus_dir}"}]
    hits: list[dict[str, Any]] = []
    exts = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp"}
    for p in root.rglob("*"):
        if p.suffix.lower() not in exts:
            continue
        try:
            h = dhash(p.read_bytes())
            d = hamming_distance(target_hash, h)
            if d <= max_hamming:
                hits.append({"path": str(p), "hamming": d})
        except Exception:
            continue
        if len(hits) >= limit:
            break
    hits.sort(key=lambda x: x["hamming"])
    return hits
