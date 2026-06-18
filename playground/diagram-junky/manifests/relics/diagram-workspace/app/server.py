#!/usr/bin/env python3
"""Minimal managed-relic server for diagram-workspace. Stores diagram.document.v0 JSON by id."""

import json
import os
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path


DATA = Path(os.environ.get("DIAGRAM_WORKSPACE_DATA", "/data"))
FILE_EXT = ".diagram.json"


def safe_id(s: str) -> str:
    return "".join(c for c in s if c.isalnum() or c in "_-.")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # suppress stderr noise inside container

    def do_GET(self):
        if self.path == "/health":
            self._respond(200, {"status": "healthy"})
        else:
            self._respond(404, {"error": "not found"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length)) if length else {}

        if self.path == "/health":
            self._respond(200, {"status": "healthy"})
        elif self.path == "/put":
            self._handle_put(body)
        elif self.path == "/get":
            self._handle_get(body)
        elif self.path == "/list":
            self._handle_list()
        elif self.path == "/delete":
            self._handle_delete(body)
        else:
            self._respond(404, {"error": "unknown endpoint"})

    def _handle_put(self, body):
        did = body.get("id")
        if not did:
            self._respond(400, {"error": "missing id"})
            return
        doc = body.get("document") or body.get("data")
        DATA.mkdir(parents=True, exist_ok=True)
        (DATA / f"{safe_id(did)}{FILE_EXT}").write_text(json.dumps(doc, indent=2))
        self._respond(200, {"ok": True})

    def _handle_get(self, body):
        did = body.get("id")
        if not did:
            self._respond(400, {"error": "missing id"})
            return
        path = DATA / f"{safe_id(did)}{FILE_EXT}"
        if not path.exists():
            self._respond(404, {"error": "not found"})
            return
        self._respond(200, {"id": did, "document": json.loads(path.read_text())})

    def _handle_list(self):
        ids = sorted(p.stem for p in DATA.glob(f"*{FILE_EXT}")) if DATA.exists() else []
        self._respond(200, {"ids": ids})

    def _handle_delete(self, body):
        did = body.get("id")
        if not did:
            self._respond(400, {"error": "missing id"})
            return
        path = DATA / f"{safe_id(did)}{FILE_EXT}"
        if path.exists():
            path.unlink()
        self._respond(200, {"ok": True})

    def _respond(self, code, payload):
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8127
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()
