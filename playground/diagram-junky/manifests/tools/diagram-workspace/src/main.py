#!/usr/bin/env python3
"""MK3 tool wrapper around the diagram_workspace relic API.

Maps a single ``action`` string to a ``WorkspaceClient`` call. The tool
returns a JSON envelope the agent can parse directly; data is whatever
the server returned, error is a short human-readable string on failure.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any


# Make the diagram_junky package importable.
_HERE = Path(__file__).resolve()
# manifests/tools/diagram-workspace/src/main.py -> playground/diagram-junky
_PKG_ROOT = _HERE.parents[4]  # 0=src, 1=diagram-workspace, 2=tools, 3=manifests, 4=diagram-junky
if str(_PKG_ROOT) not in sys.path:
    sys.path.insert(0, str(_PKG_ROOT))

from diagram_junky.client import WorkspaceClient, WorkspaceError  # noqa: E402


def read_params() -> dict[str, Any]:
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        return {}
    arg = sys.argv[1]
    # cortex-mk3 executeScriptTool writes params to a temp file and passes the
    # path; executeScript passes JSON directly. Handle both.
    if arg.startswith("/") or arg.startswith("./") or arg.startswith("../"):
        try:
            return json.loads(Path(arg).read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            pass
    return json.loads(arg)


def _fail(action: str, error: str) -> int:
    print(json.dumps({"success": False, "action": action, "error": error}, separators=(",", ":")))
    return 1


def main() -> int:
    try:
        params = read_params()
    except json.JSONDecodeError as e:
        return _fail("?", f"invalid params json: {e}")

    action = str(params.get("action", "")).strip()
    if not action:
        return _fail("?", "missing action")

    server_url = str(params.get("server_url") or os.environ.get("DIAGRAM_WORKSPACE_URL") or "http://localhost:8127")
    actor = params.get("actor") or "agent"
    client = WorkspaceClient(server_url, actor=actor, timeout=10.0)

    try:
        result = _dispatch(client, action, params)
    except WorkspaceError as e:
        return _fail(action, str(e))
    except Exception as e:  # noqa: BLE001
        return _fail(action, f"{type(e).__name__}: {e}")

    print(json.dumps({"success": True, "action": action, "data": result}, separators=(",", ":"), default=str))
    return 0


def _dispatch(client: WorkspaceClient, action: str, p: dict[str, Any]) -> Any:
    if action == "info":
        return client.info()
    if action == "health":
        return client.health()
    if action == "workspace.list":
        return {"workspaces": client.list_workspaces()}
    if action == "workspace.create":
        return client.create_workspace(workspace=p["workspace"], title=p.get("title"))
    if action == "workspace.rename":
        return client.rename_workspace(workspace=p["workspace"], title=p["title"])
    if action == "workspace.delete":
        return client.delete_workspace(workspace=p["workspace"])
    if action == "project.list":
        return {"workspace": p["workspace"], "projects": client.list_projects(workspace=p["workspace"])}
    if action == "project.create":
        return client.create_project(workspace=p["workspace"], project=p["project"], title=p.get("title"))
    if action == "project.rename":
        return client.rename_project(workspace=p["workspace"], project=p["project"], title=p["title"])
    if action == "project.delete":
        return client.delete_project(workspace=p["workspace"], project=p["project"])
    if action == "diagram.list":
        return {"diagrams": client.list_diagrams(workspace=p.get("workspace"), project=p.get("project"))}
    if action == "diagram.get":
        return client.get_diagram(id=p["id"], workspace=p.get("workspace"), project=p.get("project"))
    if action == "diagram.put":
        doc = _resolve_document(p)
        return client.put_diagram(id=p["id"], document=doc, workspace=p.get("workspace"), project=p.get("project"))
    if action == "diagram.delete":
        return client.delete_diagram(id=p["id"], workspace=p.get("workspace"), project=p.get("project"))
    if action == "diagram.render":
        doc = _resolve_document(p)
        return client.render_diagram(
            document=doc,
            width=int(p.get("width", 120)),
            height=int(p.get("height", 40)),
            theme=str(p.get("theme", "neon")),
            color=str(p.get("color", "never")),
        )
    if action == "patch.apply":
        return client.apply_patch(
            id=p["id"],
            ops=p["ops"],
            workspace=p.get("workspace"),
            project=p.get("project"),
        )
    if action == "session.get":
        return client.session_active()
    if action == "session.set":
        return client.set_session_active(
            workspace=p.get("workspace"),
            project=p.get("project"),
            diagram=p.get("id"),
        )
    if action == "events.tail":
        resp = client.events(since=int(p.get("since", 0)), limit=int(p.get("limit", 50)))
        return resp
    raise ValueError(f"unknown action: {action!r}")


def _resolve_document(params: dict[str, Any]) -> dict[str, Any]:
    doc = params.get("document")
    if doc is not None:
        return doc
    sp = params.get("source_path")
    if sp:
        return json.loads(Path(sp).read_text())
    raise ValueError("either document or source_path is required")


if __name__ == "__main__":
    raise SystemExit(main())
