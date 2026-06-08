#!/usr/bin/env python3
"""Web search tool — uses curl to query DuckDuckGo HTML search."""

import sys
import json
import subprocess
import urllib.parse
import re
import html

def search(query, num_results=10, domains=""):
    """Perform a web search via DuckDuckGo HTML."""
    # Add domain filters if specified
    if domains:
        for domain in domains.split(","):
            domain = domain.strip()
            if domain:
                query += f" site:{domain}"

    url = f"https://html.duckduckgo.com/html/?q={urllib.parse.quote_plus(query)}"

    try:
        result = subprocess.run(
            ["curl", "-s", "-L", "-A", "Mozilla/5.0", url],
            capture_output=True, text=True, timeout=30
        )
        page = result.stdout
    except Exception as e:
        return json.dumps({"error": str(e), "results": []})

    # Parse DuckDuckGo HTML results
    results = []
    # DDG HTML uses class="result" divs with <a class="result__a"> for title/url
    # and <a class="result__snippet"> for snippets
    title_pattern = re.compile(r'<a[^>]*class="result__a"[^>]*href="([^"]*)"[^>]*>(.*?)</a>', re.DOTALL)
    snippet_pattern = re.compile(r'<a[^>]*class="result__snippet"[^>]*>(.*?)</a>', re.DOTALL)

    titles = title_pattern.findall(page)
    snippets = snippet_pattern.findall(page)

    for i, (url_match, title_match) in enumerate(titles):
        if i >= num_results:
            break
        # Clean HTML from title
        title = re.sub(r'<[^>]+>', '', title_match)
        title = html.unescape(title).strip()

        # DDG redirects through their own URL — extract the actual URL
        actual_url = url_match
        if "uddg=" in url_match:
            try:
                actual_url = urllib.parse.unquote(
                    url_match.split("uddg=")[1].split("&")[0]
                )
            except (IndexError, ValueError):
                pass

        # Get snippet if available
        snippet = ""
        if i < len(snippets):
            snippet = re.sub(r'<[^>]+>', '', snippets[i][0])
            snippet = html.unescape(snippet).strip()

        results.append({
            "title": title,
            "url": actual_url,
            "snippet": snippet
        })

    return json.dumps({
        "results": results,
        "count": len(results),
        "query": query
    }, indent=2)

if __name__ == "__main__":
    try:
        params = json.loads(sys.stdin.read())
    except json.JSONDecodeError:
        print(json.dumps({"error": "Invalid JSON input"}))
        sys.exit(1)

    query = params.get("query", "")
    num_results = params.get("num_results", 10)
    domains = params.get("domains", "")

    if not query:
        print(json.dumps({"error": "query is required"}))
        sys.exit(1)

    print(search(query, num_results, domains))
