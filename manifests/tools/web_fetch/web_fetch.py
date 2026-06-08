#!/usr/bin/env python3
"""web_fetch — Fetch content from a URL."""
import json
import sys
import urllib.request
import urllib.error

def main():
    try:
        params = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    except (json.JSONDecodeError, ValueError) as e:
        print(json.dumps({"error": f"invalid JSON input: {e}"}))
        sys.exit(1)
    url = params.get("url", "")
    method = params.get("method", "GET").upper()
    headers = params.get("headers", {})
    body = params.get("body", "")
    timeout = int(params.get("timeout", 30))

    if not url:
        print(json.dumps({"error": "url is required"}))
        sys.exit(1)

    try:
        req = urllib.request.Request(url, method=method)
        for k, v in headers.items():
            req.add_header(k, v)
        if body and method == "POST":
            req.data = body.encode("utf-8")
            if "Content-Type" not in headers:
                req.add_header("Content-Type", "application/json")

        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status = resp.status
            resp_headers = dict(resp.headers)
            raw = resp.read()
            # Try decode as utf-8, fallback to latin-1
            try:
                content = raw.decode("utf-8")
            except UnicodeDecodeError:
                content = raw.decode("latin-1")

            print(json.dumps({
                "status": status,
                "headers": resp_headers,
                "body": content
            }))

    except urllib.error.HTTPError as e:
        body_text = ""
        try:
            body_text = e.read().decode("utf-8", errors="replace")
        except Exception:
            pass
        print(json.dumps({
            "status": e.code,
            "headers": dict(e.headers) if e.headers else {},
            "body": body_text
        }))
    except urllib.error.URLError as e:
        print(json.dumps({"error": f"connection failed: {e.reason}"}))
        sys.exit(1)
    except Exception as e:
        print(json.dumps({"error": str(e)}))
        sys.exit(1)

if __name__ == "__main__":
    main()
