"""Data-source registry + adapters for the discovery agent.

`get_data_source_desc` renders the catalog (data_sources.json).
`get_data_source` dispatches to a per-source adapter below.

Every adapter is a read-only GET against a public, no-key (or public-key)
REST API. No writes, no state, no side effects.
"""

from __future__ import annotations

import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any
from urllib.parse import quote_plus

from common import http_get, http_get_text, _clean

_CATALOG_PATH = Path(__file__).with_name("data_sources.json")

# arXiv Atom namespaces
_ATOM = {"atom": "http://www.w3.org/2005/Atom"}


def load_catalog() -> list[dict[str, Any]]:
    return json.loads(_CATALOG_PATH.read_text())["sources"]


def describe_source(source_id: str) -> dict[str, Any] | None:
    for s in load_catalog():
        if s["id"] == source_id:
            return s
    return None


# --------------------------------------------------------------------------
# Adapters — each returns {success, source, results:[...], ...}
# --------------------------------------------------------------------------
def _arxiv(query: str, max_results: int) -> dict[str, Any]:
    url = "https://export.arxiv.org/api/query"
    params = {
        "search_query": query,
        "start": "0",
        "max_results": str(max_results),
    }
    try:
        _status, text, _final, _ct = http_get_text(url, params=params)
    except Exception as e:
        return {"success": False, "error": str(e), "results": []}

    results = []
    try:
        root = ET.fromstring(text)
        for entry in root.findall("atom:entry", _ATOM):
            title = (entry.findtext("atom:title", "", _ATOM) or "").strip()
            summary = (entry.findtext("atom:summary", "", _ATOM) or "").strip()
            url_entry = entry.findtext("atom:id", "", _ATOM) or ""
            authors = [
                a.findtext("atom:name", "", _ATOM)
                for a in entry.findall("atom:author", _ATOM)
            ]
            published = entry.findtext("atom:published", "", _ATOM) or ""
            results.append({
                "title": re.sub(r"\s+", " ", title),
                "url": url_entry,
                "authors": [a for a in authors if a],
                "summary": re.sub(r"\s+", " ", summary)[:600],
                "published": published,
            })
    except ET.ParseError as e:
        return {"success": False, "error": f"arxiv parse error: {e}",
                "results": []}
    return {"success": True, "source": "arxiv", "results": results,
            "count": len(results)}


def _crossref(query: str, rows: int) -> dict[str, Any]:
    url = "https://api.crossref.org/works"
    params = {"query": query, "rows": str(rows),
              "select": "DOI,title,container-title,issued,author,URL"}
    try:
        _status, text, _final, _ct = http_get_text(url, params=params)
        data = json.loads(text)
    except Exception as e:
        return {"success": False, "error": str(e), "results": []}
    results = []
    for it in data.get("message", {}).get("items", []):
        authors = [
            (f"{a.get('given', '')} {a.get('family', '')}").strip()
            for a in it.get("author", [])
        ]
        results.append({
            "title": (it.get("title") or [""])[0],
            "url": it.get("URL", ""),
            "container": (it.get("container-title") or [""])[0],
            "issued": (it.get("issued", {}).get("date-parts") or [[None]])[0][0],
            "authors": [a for a in authors if a][:8],
            "doi": it.get("DOI", ""),
        })
    return {"success": True, "source": "crossref", "results": results,
            "count": len(results)}


def _openalex(query: str, per_page: int) -> dict[str, Any]:
    url = "https://api.openalex.org/works"
    params = {"search": query, "per-page": str(per_page)}
    try:
        _status, text, _final, _ct = http_get_text(url, params=params)
        data = json.loads(text)
    except Exception as e:
        return {"success": False, "error": str(e), "results": []}
    results = []
    for w in data.get("results", []):
        results.append({
            "title": w.get("display_name", ""),
            "url": w.get("id", ""),
            "publication_year": w.get("publication_year"),
            "cited_by_count": w.get("cited_by_count"),
            "doi": (w.get("doi") or "").replace("https://doi.org/", ""),
        })
    return {"success": True, "source": "openalex", "results": results,
            "count": len(results)}


def _wikipedia(query: str, mode: str, limit: int) -> dict[str, Any]:
    api = "https://en.wikipedia.org/w/api.php"
    if mode == "summary":
        params = {
            "action": "query", "prop": "extracts", "exintro": "1",
            "explaintext": "1", "redirects": "1", "titles": query,
            "format": "json",
        }
        try:
            _status, text, _final, _ct = http_get_text(api, params=params)
            data = json.loads(text)
        except Exception as e:
            return {"success": False, "error": str(e), "results": []}
        results = []
        for page in data.get("query", {}).get("pages", {}).values():
            if "missing" in page:
                continue
            results.append({
                "title": page.get("title", ""),
                "pageid": page.get("pageid"),
                "url": f"https://en.wikipedia.org/wiki/{quote_plus(page.get('title', '').replace(' ', '_'))}",
                "extract": (page.get("extract") or "").strip(),
            })
        return {"success": True, "source": "wikipedia", "results": results,
                "count": len(results)}

    # search / fulltext
    params = {
        "action": "query", "list": "search", "srsearch": query,
        "srlimit": str(limit), "format": "json",
    }
    if mode == "fulltext":
        params["srsearch"] = query
        params["srwhat"] = "text"
    try:
        _status, text, _final, _ct = http_get_text(api, params=params)
        data = json.loads(text)
    except Exception as e:
        return {"success": False, "error": str(e), "results": []}
    results = []
    for r in data.get("query", {}).get("search", []):
        results.append({
            "title": r.get("title", ""),
            "url": f"https://en.wikipedia.org/wiki/{quote_plus(r.get('title', '').replace(' ', '_'))}",
            "extract": re.sub(r"<[^>]+>", "", r.get("snippet", "")),
            "pageid": r.get("pageid"),
        })
    return {"success": True, "source": "wikipedia", "results": results,
            "count": len(results)}


def _yahoo(symbol: str) -> dict[str, Any]:
    import datetime as _dt
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{quote_plus(symbol)}"
    params = {"range": "1d", "interval": "1d"}
    headers = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64)"}
    try:
        _status, text, _final, _ct = http_get_text(url, params=params,
                                                   headers=headers)
        data = json.loads(text)
        result = data["chart"]["result"][0]
    except Exception as e:
        return {"success": False, "error": str(e), "results": []}

    meta = result.get("meta", {})
    quote = (result.get("indicators", {}).get("quote") or [{}])[0]

    def _first(seq):
        if isinstance(seq, list) and seq:
            return seq[0]
        return None

    out = {
        "symbol": meta.get("symbol", symbol),
        "name": meta.get("shortName") or meta.get("longName", ""),
        "currency": meta.get("currency", ""),
        "exchange": meta.get("fullExchangeName", ""),
        "price": meta.get("regularMarketPrice"),
        "previous_close": meta.get("chartPreviousClose"),
        "open": _first(quote.get("open")),
        "day_high": meta.get("regularMarketDayHigh") or _first(quote.get("high")),
        "day_low": meta.get("regularMarketDayLow") or _first(quote.get("low")),
        "volume": meta.get("regularMarketVolume") or _first(quote.get("volume")),
        "fifty_two_week_high": meta.get("fiftyTwoWeekHigh"),
        "fifty_two_week_low": meta.get("fiftyTwoWeekLow"),
        "market_time": (
            _dt.datetime.fromtimestamp(meta["regularMarketTime"], tz=_dt.timezone.utc)
            .isoformat() if meta.get("regularMarketTime") else ""
        ),
    }
    return {"success": True, "source": "yahoo", "results": [out],
            "count": 1}


def _github(query: str, per_page: int) -> dict[str, Any]:
    url = "https://api.github.com/search/repositories"
    params = {"q": query, "per_page": str(per_page)}
    headers = {"Accept": "application/vnd.github+json"}
    token = __import__("os").environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    try:
        _status, text, _final, _ct = http_get_text(url, params=params,
                                                   headers=headers)
        data = json.loads(text)
    except Exception as e:
        return {"success": False, "error": str(e), "results": []}
    results = []
    for r in data.get("items", []):
        results.append({
            "full_name": r.get("full_name", ""),
            "url": r.get("html_url", ""),
            "description": (r.get("description") or "")[:300],
            "stars": r.get("stargazers_count"),
            "language": r.get("language"),
            "updated_at": r.get("updated_at", ""),
        })
    return {"success": True, "source": "github", "results": results,
            "count": len(results)}


_ADAPTERS = {
    "arxiv": _arxiv,
    "crossref": _crossref,
    "openalex": _openalex,
    "wikipedia": _wikipedia,
    "yahoo": _yahoo,
    "github": _github,
}


def query_source(source_id: str, params: dict[str, Any]) -> dict[str, Any]:
    adapter = _ADAPTERS.get(source_id)
    if adapter is None:
        known = ", ".join(sorted(_ADAPTERS))
        return {"success": False,
                "error": f"unknown source '{source_id}'. Known: {known}",
                "results": []}
    return adapter(**params)
