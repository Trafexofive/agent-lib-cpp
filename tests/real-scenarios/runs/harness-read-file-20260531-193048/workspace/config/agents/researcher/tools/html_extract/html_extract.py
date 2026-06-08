#!/usr/bin/env python3
"""HTML content extractor — fetches URL and extracts readable content."""

import sys
import json
import subprocess
import re
import html as html_mod

def extract_content(url, selector="", max_length=10000):
    """Fetch URL and extract readable content."""
    try:
        result = subprocess.run(
            ["curl", "-s", "-L", "-A", "Mozilla/5.0", "--max-time", "25", url],
            capture_output=True, text=True, timeout=30
        )
        page = result.stdout
    except Exception as e:
        return json.dumps({"error": str(e), "url": url})

    if not page:
        return json.dumps({"error": "Empty response", "url": url})

    # Extract title
    title = ""
    title_match = re.search(r'<title[^>]*>(.*?)</title>', page, re.DOTALL | re.IGNORECASE)
    if title_match:
        title = re.sub(r'<[^>]+>', '', title_match.group(1)).strip()
        title = html_mod.unescape(title)

    # If selector specified, extract that section
    content = page
    if selector:
        # Simple tag-based extraction (no CSS parser needed)
        pattern = re.compile(
            rf'<{re.escape(selector)}[^>]*>(.*?)</{re.escape(selector)}>',
            re.DOTALL | re.IGNORECASE
        )
        matches = pattern.findall(page)
        if matches:
            content = "\n".join(matches)

    # Strip all HTML tags
    text = re.sub(r'<script[^>]*>.*?</script>', '', content, flags=re.DOTALL | re.IGNORECASE)
    text = re.sub(r'<style[^>]*>.*?</style>', '', text, flags=re.DOTALL | re.IGNORECASE)
    text = re.sub(r'<nav[^>]*>.*?</nav>', '', text, flags=re.DOTALL | re.IGNORECASE)
    text = re.sub(r'<footer[^>]*>.*?</footer>', '', text, flags=re.DOTALL | re.IGNORECASE)
    text = re.sub(r'<header[^>]*>.*?</header>', '', text, flags=re.DOTALL | re.IGNORECASE)
    text = re.sub(r'<[^>]+>', ' ', text)
    text = html_mod.unescape(text)

    # Clean up whitespace
    text = re.sub(r'[ \t]+', ' ', text)
    text = re.sub(r'\n\s*\n\s*\n', '\n\n', text)
    text = text.strip()

    # Truncate if needed
    truncated = False
    if len(text) > max_length:
        text = text[:max_length]
        truncated = True

    return json.dumps({
        "title": title,
        "content": text,
        "url": url,
        "length": len(text),
        "truncated": truncated
    }, indent=2)

if __name__ == "__main__":
    try:
        params = json.loads(sys.stdin.read())
    except json.JSONDecodeError:
        print(json.dumps({"error": "Invalid JSON input"}))
        sys.exit(1)

    url = params.get("url", "")
    selector = params.get("selector", "")
    max_length = params.get("max_length", 10000)

    if not url:
        print(json.dumps({"error": "url is required"}))
        sys.exit(1)

    print(extract_content(url, selector, max_length))
