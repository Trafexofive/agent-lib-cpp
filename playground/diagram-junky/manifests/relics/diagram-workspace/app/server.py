#!/usr/bin/env python3
"""Managed relic server for diagram-junky workspaces.

Stores standalone diagram.document.v0 JSON by id and provides a small
workspace/project hierarchy for the TUI harness client.
"""

import json
import os
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse


DATA = Path(os.environ.get("DIAGRAM_WORKSPACE_DATA", "/data"))
FILE_EXT = ".diagram.json"


def safe_id(s: str) -> str:
    cleaned = "".join(c for c in str(s) if c.isalnum() or c in "_-.").strip(".")
    return cleaned or "untitled"


def now() -> float:
    return time.time()


def workspace_dir(workspace: str) -> Path:
    return DATA / "workspaces" / safe_id(workspace)


def project_dir(workspace: str, project: str) -> Path:
    return workspace_dir(workspace) / "projects" / safe_id(project)


def diagrams_dir(workspace: str, project: str) -> Path:
    return project_dir(workspace, project) / "diagrams"


def write_json(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2))


def read_json(path: Path, default=None):
    if not path.exists():
        return default
    return json.loads(path.read_text())


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/health":
            self._respond(200, {"status": "healthy"})
        else:
            self._respond(404, {"error": "not found"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(length)) if length else {}
        except json.JSONDecodeError as e:
            self._respond(400, {"error": f"invalid json: {e}"})
            return
        path = urlparse(self.path).path

        routes = {
            "/health": self._handle_health,
            "/put": self._handle_put,
            "/get": self._handle_get,
            "/list": self._handle_list,
            "/delete": self._handle_delete,
            "/workspace/create": self._workspace_create,
            "/workspace/list": self._workspace_list,
            "/project/create": self._project_create,
            "/project/list": self._project_list,
            "/project/copy": self._project_copy,
            "/project/diagrams": self._project_diagrams,
        }
        handler = routes.get(path)
        if not handler:
            self._respond(404, {"error": "unknown endpoint"})
            return
        handler(body)

    def _handle_health(self, _body):
        self._respond(200, {"status": "healthy"})

    def _handle_put(self, body):
        did = body.get("id")
        if not did:
            self._respond(400, {"error": "missing id"})
            return
        doc = body.get("document") or body.get("data")
        DATA.mkdir(parents=True, exist_ok=True)
        write_json(DATA / f"{safe_id(did)}{FILE_EXT}", doc)
        self._respond(200, {"ok": True, "id": safe_id(did)})

    def _handle_get(self, body):
        did = body.get("id")
        if not did:
            self._respond(400, {"error": "missing id"})
            return
        path = DATA / f"{safe_id(did)}{FILE_EXT}"
        if not path.exists():
            self._respond(404, {"error": "not found"})
            return
        self._respond(200, {"id": safe_id(did), "document": read_json(path)})

    def _handle_list(self, _body=None):
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
        self._respond(200, {"ok": True, "id": safe_id(did)})

    def _workspace_create(self, body):
        wid = safe_id(body.get("workspace") or body.get("id") or f"workspace-{int(now())}")
        root = workspace_dir(wid)
        root.mkdir(parents=True, exist_ok=True)
        meta = read_json(root / "workspace.json", {}) or {}
        meta.update({"id": wid, "title": body.get("title", wid), "updated_at": now()})
        meta.setdefault("created_at", now())
        write_json(root / "workspace.json", meta)
        self._respond(200, {"ok": True, "workspace": meta})

    def _workspace_list(self, _body):
        root = DATA / "workspaces"
        workspaces = []
        if root.exists():
            for path in sorted(root.iterdir()):
                if path.is_dir():
                    workspaces.append(read_json(path / "workspace.json", {"id": path.name, "title": path.name}))
        self._respond(200, {"workspaces": workspaces})

    def _project_create(self, body):
        wid = safe_id(body.get("workspace") or "default")
        pid = safe_id(body.get("project") or body.get("id") or f"project-{int(now())}")
        root = project_dir(wid, pid)
        diagrams_dir(wid, pid).mkdir(parents=True, exist_ok=True)
        meta = read_json(root / "project.json", {}) or {}
        meta.update({"id": pid, "workspace": wid, "title": body.get("title", pid), "updated_at": now()})
        meta.setdefault("created_at", now())
        write_json(root / "project.json", meta)
        self._respond(200, {"ok": True, "project": meta})

    def _project_list(self, body):
        wid = safe_id(body.get("workspace") or "default")
        root = workspace_dir(wid) / "projects"
        projects = []
        if root.exists():
            for path in sorted(root.iterdir()):
                if path.is_dir():
                    projects.append(read_json(path / "project.json", {"id": path.name, "workspace": wid, "title": path.name}))
        self._respond(200, {"workspace": wid, "projects": projects})

    def _project_copy(self, body):
        wid = safe_id(body.get("workspace") or "default")
        pid = safe_id(body.get("project") or "inbox")
        doc = body.get("document") or body.get("data")
        source_path = body.get("source_path")
        if doc is None and source_path:
            p = Path(source_path)
            if not p.exists():
                self._respond(404, {"error": f"source_path not found: {source_path}"})
                return
            doc = read_json(p)
        if doc is None:
            self._respond(400, {"error": "document or source_path required"})
            return
        did = safe_id(body.get("id") or doc.get("id") or f"diagram-{int(now())}")
        target = diagrams_dir(wid, pid) / f"{did}{FILE_EXT}"
        write_json(target, doc)
        self._respond(200, {"ok": True, "workspace": wid, "project": pid, "id": did, "path": str(target)})

    def _project_diagrams(self, body):
        wid = safe_id(body.get("workspace") or "default")
        pid = safe_id(body.get("project") or "inbox")
        root = diagrams_dir(wid, pid)
        diagrams = []
        if root.exists():
            for p in sorted(root.glob(f"*{FILE_EXT}")):
                doc = read_json(p, {}) or {}
                diagrams.append({"id": p.name.removesuffix(FILE_EXT), "title": doc.get("title", p.stem), "path": str(p)})
        self._respond(200, {"workspace": wid, "project": pid, "diagrams": diagrams})

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
