#!/usr/bin/env python3
"""Managed relic server for diagram-junky workspaces.

Exposes the diagram_workspace relic API: workspaces, projects, diagram
documents, an event log, server-sent events for real-time clients, an
active-session pointer for follow mode, and file locks for multi-process
safety. Designed to be a single managed instance on localhost that
multiple agents and TUI clients can attach to concurrently.

Endpoints are POST-with-JSON-body by default. /health, /version, /info,
/events/stream, and /events GET endpoints are read-only. All mutations
append an event to events.jsonl so clients can stream the activity log
in real time.
"""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import json
import os
import queue
import re
import sys
import threading
import time
import uuid
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Iterable
from urllib.parse import parse_qs, urlparse

# Make the diagram_junky package importable from a local checkout, where
# server.py lives at playground/diagram-junky/manifests/relics/diagram-workspace/app/server.py
# and the package is at the diagram-junky workspace root (parents[5]).
# The Docker image bundles the package in /app/diagram_junky, so this is
# a no-op there.
for _candidate in (Path(__file__).resolve().parents[5], Path("/app")):
    if (_candidate / "diagram_junky").is_dir() and str(_candidate) not in sys.path:
        sys.path.insert(0, str(_candidate))


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DATA = Path(os.environ.get("DIAGRAM_WORKSPACE_DATA", "/data"))
LOCK_TTL_SEC = 30.0
EVENT_BUFFER_SIZE = 1024
SERVER_VERSION = "0.2.0"
STARTED_AT = time.time()

FILE_EXT = ".diagram.json"
META_EXT = ".meta.json"

ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:\- ]{0,127}$")
SAFE_RE = re.compile(r"[^A-Za-z0-9_.\-:]")


# ---------------------------------------------------------------------------
# Storage layout
# ---------------------------------------------------------------------------


def safe_id(value: str | None, *, fallback: str | None = None) -> str:
    """Normalize an identifier for safe filesystem use.

    Keeps alphanumerics, dot, dash, underscore, colon. Spaces are kept
    because diagrams are human-named — the file system tolerates them
    and we want friendlier UX. Other chars collapse to '_'.
    """
    if not value:
        return fallback or f"untitled-{uuid.uuid4().hex[:8]}"
    cleaned = SAFE_RE.sub("_", str(value).strip()).strip("._")
    return cleaned or (fallback or f"untitled-{uuid.uuid4().hex[:8]}")


def validate_id(value: str | None, *, kind: str) -> str | None:
    if value is None:
        return f"missing {kind}"
    if not isinstance(value, str):
        return f"{kind} must be a string"
    if not ID_RE.match(value):
        return f"invalid {kind}: {value!r}"
    return None


def now() -> float:
    return time.time()


def workspace_dir(workspace: str) -> Path:
    return DATA / "workspaces" / safe_id(workspace)


def project_dir(workspace: str, project: str) -> Path:
    return workspace_dir(workspace) / "projects" / safe_id(project)


def diagrams_dir(workspace: str, project: str) -> Path:
    return project_dir(workspace, project) / "diagrams"


def standalone_dir() -> Path:
    return DATA / "diagrams"


def events_path() -> Path:
    return DATA / "events.jsonl"


def session_path() -> Path:
    return DATA / "session.json"


def lock_path(resource: str) -> Path:
    return DATA / "locks" / f"{resource}.lock"


# ---------------------------------------------------------------------------
# Atomic file helpers
# ---------------------------------------------------------------------------


def write_json_atomic(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True))
    os.replace(tmp, path)


def read_json(path: Path, default: Any = None) -> Any:
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError:
        return default


# ---------------------------------------------------------------------------
# File locks (advisory, with TTL)
# ---------------------------------------------------------------------------


@dataclass
class LockEntry:
    holder: str
    acquired_at: float
    refreshed_at: float


class LockTable:
    """Per-process in-memory lock registry. Released on process exit.

    We hold the OS file lock for the lifetime of the in-memory entry. If
    the process crashes, the OS releases the fd and the entry expires
    after LOCK_TTL_SEC so a subsequent caller can take over.
    """

    def __init__(self, ttl: float) -> None:
        self.ttl = ttl
        self._entries: dict[str, tuple[LockEntry, Any]] = {}
        self._lock = threading.Lock()
        self._root = DATA / "locks"
        self._root.mkdir(parents=True, exist_ok=True)

    def _gc(self) -> None:
        t = time.time()
        expired = [
            k for k, (entry, _) in self._entries.items()
            if t - entry.refreshed_at > self.ttl
        ]
        for k in expired:
            entry, fd = self._entries.pop(k, (None, None))
            if fd is not None:
                with contextlib.suppress(OSError):
                    os.close(fd)
            # Best-effort cleanup of the lock file.
            with contextlib.suppress(FileNotFoundError):
                lock_path(k).unlink()

    def acquire(self, resource: str, holder: str) -> tuple[bool, str | None]:
        with self._lock:
            self._gc()
            if resource in self._entries:
                entry, _ = self._entries[resource]
                if entry.holder != holder and time.time() - entry.refreshed_at <= self.ttl:
                    return False, entry.holder
            p = lock_path(resource)
            p.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(str(p), os.O_CREAT | os.O_RDWR, 0o666)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except (BlockingIOError, OSError):
                os.close(fd)
                return False, None
            self._entries[resource] = (
                LockEntry(holder=holder, acquired_at=time.time(), refreshed_at=time.time()),
                fd,
            )
            return True, None

    def release(self, resource: str, holder: str) -> None:
        with self._lock:
            entry = self._entries.get(resource)
            if not entry or entry[0].holder != holder:
                return
            _, fd = self._entries.pop(resource)
            with contextlib.suppress(OSError):
                os.close(fd)
            with contextlib.suppress(FileNotFoundError):
                lock_path(resource).unlink()


# ---------------------------------------------------------------------------
# Event log
# ---------------------------------------------------------------------------


@dataclass
class Event:
    seq: int
    ts: float
    kind: str
    actor: str
    workspace: str | None
    project: str | None
    diagram: str | None
    summary: str
    data: dict[str, Any]


class EventBus:
    """Append-only event log + in-process pub/sub for SSE subscribers."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._seq = self._load_last_seq()
        self._subscribers: list[queue.Queue[Event | None]] = []
        self._sub_lock = threading.Lock()

    def _load_last_seq(self) -> int:
        last = 0
        if self.path.exists():
            with contextlib.suppress(OSError):
                for line in self.path.read_text().splitlines():
                    if not line.strip():
                        continue
                    with contextlib.suppress(json.JSONDecodeError):
                        last = max(last, int(json.loads(line).get("seq", 0)))
        return last

    def next_seq(self) -> int:
        with self._lock:
            self._seq += 1
            return self._seq

    def emit(self, event: Event) -> Event:
        line = json.dumps(event.__dict__, sort_keys=True, separators=(",", ":"))
        with self._lock:
            with self.path.open("a", encoding="utf-8") as f:
                fcntl.flock(f.fileno(), fcntl.LOCK_EX)
                f.write(line + "\n")
                f.flush()
        with self._sub_lock:
            dead: list[queue.Queue[Event | None]] = []
            for q in self._subscribers:
                try:
                    q.put_nowait(event)
                except queue.Full:
                    dead.append(q)
            for q in dead:
                with contextlib.suppress(ValueError):
                    self._subscribers.remove(q)
        return event

    def subscribe(self) -> queue.Queue[Event | None]:
        q: queue.Queue[Event | None] = queue.Queue(maxsize=EVENT_BUFFER_SIZE)
        with self._sub_lock:
            self._subscribers.append(q)
        return q

    def unsubscribe(self, q: queue.Queue[Event | None]) -> None:
        with self._sub_lock:
            with contextlib.suppress(ValueError):
                self._subscribers.remove(q)

    def tail(self, since: int, limit: int = 500) -> list[Event]:
        if not self.path.exists():
            return []
        out: list[Event] = []
        with self.path.open("r", encoding="utf-8") as f:
            for line in f:
                if not line.strip():
                    continue
                with contextlib.suppress(json.JSONDecodeError):
                    raw = json.loads(line)
                    if int(raw.get("seq", 0)) > since:
                        out.append(Event(**raw))
                        if len(out) >= limit:
                            break
        return out


# ---------------------------------------------------------------------------
# Workspace / project / diagram catalog
# ---------------------------------------------------------------------------


def list_workspaces() -> list[dict[str, Any]]:
    root = DATA / "workspaces"
    if not root.exists():
        return []
    out: list[dict[str, Any]] = []
    for p in sorted(root.iterdir()):
        if not p.is_dir():
            continue
        meta = read_json(p / "workspace.json", {"id": p.name, "title": p.name})
        meta.setdefault("id", p.name)
        out.append(meta)
    return out


def list_projects(workspace: str) -> list[dict[str, Any]]:
    root = workspace_dir(workspace) / "projects"
    if not root.exists():
        return []
    out: list[dict[str, Any]] = []
    for p in sorted(root.iterdir()):
        if not p.is_dir():
            continue
        meta = read_json(p / "project.json", {"id": p.name, "workspace": workspace, "title": p.name})
        meta.setdefault("id", p.name)
        meta.setdefault("workspace", workspace)
        out.append(meta)
    return out


def list_diagrams(workspace: str, project: str) -> list[dict[str, Any]]:
    root = diagrams_dir(workspace, project)
    if not root.exists():
        return []
    out: list[dict[str, Any]] = []
    for p in sorted(root.glob(f"*{FILE_EXT}")):
        meta_path = p.with_suffix(".meta.json")
        meta = read_json(meta_path, {})
        doc = read_json(p, {})
        out.append(
            {
                "id": p.name.removesuffix(FILE_EXT),
                "title": doc.get("title") or meta.get("title") or p.stem,
                "kind": doc.get("kind", "unknown"),
                "nodes": len(doc.get("nodes", [])) if isinstance(doc, dict) else 0,
                "edges": len(doc.get("edges", [])) if isinstance(doc, dict) else 0,
                "path": str(p),
                "updated_at": meta.get("updated_at"),
            }
        )
    return out


def list_standalone() -> list[dict[str, Any]]:
    root = standalone_dir()
    if not root.exists():
        return []
    out: list[dict[str, Any]] = []
    for p in sorted(root.glob(f"*{FILE_EXT}")):
        meta_path = p.with_suffix(".meta.json")
        meta = read_json(meta_path, {})
        doc = read_json(p, {})
        out.append(
            {
                "id": p.name.removesuffix(FILE_EXT),
                "title": doc.get("title") or meta.get("title") or p.stem,
                "kind": doc.get("kind", "unknown"),
                "nodes": len(doc.get("nodes", [])) if isinstance(doc, dict) else 0,
                "edges": len(doc.get("edges", [])) if isinstance(doc, dict) else 0,
                "path": str(p),
                "updated_at": meta.get("updated_at"),
            }
        )
    return out


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def validate_document(doc: Any) -> tuple[bool, str | None]:
    if not isinstance(doc, dict):
        return False, "document must be an object"
    if doc.get("schema_version") != "diagram.document.v0":
        return False, f"unsupported schema_version: {doc.get('schema_version')!r}"
    if not isinstance(doc.get("nodes"), list):
        return False, "document.nodes must be a list"
    if not isinstance(doc.get("edges"), list):
        return False, "document.edges must be a list"
    return True, None


def _validate(doc: Any) -> str | None:
    """Single-error variant of validate_document, for inline use."""
    ok, err = validate_document(doc)
    return err if not ok else None


# ---------------------------------------------------------------------------
# Rendering helper (uses the playground renderer if importable)
# ---------------------------------------------------------------------------


def render_diagram(doc: dict[str, Any], width: int, height: int, theme: str, color: str) -> dict[str, Any]:
    try:
        from diagram_junky.rendering import Renderer  # type: ignore
    except Exception:
        return {"rendered": "", "error": "renderer module not available in this image"}
    width = max(20, min(int(width), 400))
    height = max(8, min(int(height), 200))
    theme = theme if theme in {"default", "mono", "neon"} else "default"
    color_on = color == "always" or (color == "auto" and sys.stdout.isatty())
    rendered = Renderer(doc, width, height, color=color_on, theme=theme).render()
    return {"rendered": rendered, "width": width, "height": height, "theme": theme}


# ---------------------------------------------------------------------------
# HTTP layer
# ---------------------------------------------------------------------------


BUS = EventBus(events_path())
LOCKS = LockTable(ttl=LOCK_TTL_SEC)


class Handler(BaseHTTPRequestHandler):
    server_version = f"diagram_workspace/{SERVER_VERSION}"
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A002 - stdlib signature
        # Quiet by default; enable with DIAGRAM_WORKSPACE_VERBOSE=1.
        if os.environ.get("DIAGRAM_WORKSPACE_VERBOSE"):
            sys.stderr.write(f"{self.address_string()} {format % args}\n")

    # ---------- dispatch ----------

    def do_GET(self) -> None:  # noqa: N802
        url = urlparse(self.path)
        qs = {k: v[0] for k, v in parse_qs(url.query).items()}
        routes: dict[str, Callable[[dict[str, str]], None]] = {
            "/health": self._health,
            "/version": self._version,
            "/info": self._info,
            "/events": self._events,
            "/events/stream": self._events_stream,
            "/session/active": self._session_get,
        }
        handler = routes.get(url.path)
        if not handler:
            self._json(404, {"error": "unknown endpoint", "path": url.path})
            return
        try:
            handler(qs)
        except Exception as e:  # noqa: BLE001 - surface any error to client
            self._json(500, {"error": f"{type(e).__name__}: {e}"})

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(length)) if length else {}
        except json.JSONDecodeError as e:
            self._json(400, {"error": f"invalid json: {e}"})
            return
        if not isinstance(body, dict):
            self._json(400, {"error": "body must be a json object"})
            return
        url = urlparse(self.path)
        routes: dict[str, Callable[[dict[str, Any]], None]] = {
            # health + utility
            "/health": lambda b: self._health({}),
            "/version": lambda b: self._version({}),
            # workspaces
            "/workspace/create": self._workspace_create,
            "/workspace/rename": self._workspace_rename,
            "/workspace/delete": self._workspace_delete,
            "/workspace/list": lambda b: self._workspace_list(),
            # projects
            "/project/create": self._project_create,
            "/project/rename": self._project_rename,
            "/project/delete": self._project_delete,
            "/project/list": self._project_list,
            "/project/copy": self._project_copy,
            "/project/diagrams": self._project_diagrams,
            # diagrams (unified)
            "/diagram/list": self._diagram_list,
            "/diagram/put": self._diagram_put,
            "/diagram/get": self._diagram_get,
            "/diagram/delete": self._diagram_delete,
            "/diagram/render": self._diagram_render,
            # patches
            "/patch/apply": self._patch_apply,
            # active session pointer
            "/session/active": self._session_set,
            # lock management
            "/lock/acquire": self._lock_acquire,
            "/lock/release": self._lock_release,
        }
        handler = routes.get(url.path)
        if not handler:
            self._json(404, {"error": "unknown endpoint", "path": url.path})
            return
        try:
            handler(body)
        except Exception as e:  # noqa: BLE001
            self._json(500, {"error": f"{type(e).__name__}: {e}"})

    # ---------- response helpers ----------

    def _json(self, code: int, payload: Any) -> None:
        data = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _sse(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        self._sse_open = True

    def _sse_send(self, data: str) -> None:
        if not getattr(self, "_sse_open", False):
            return
        try:
            self.wfile.write(f"data: {data}\n\n".encode("utf-8"))
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            self._sse_open = False

    # ---------- get routes ----------

    def _health(self, _qs: dict[str, str]) -> None:
        self._json(200, {"status": "healthy", "version": SERVER_VERSION, "uptime": time.time() - STARTED_AT})

    def _version(self, _qs: dict[str, str]) -> None:
        self._json(200, {"version": SERVER_VERSION, "started_at": STARTED_AT, "data_dir": str(DATA)})

    def _info(self, _qs: dict[str, str]) -> None:
        ws = list_workspaces()
        projects = {w["id"]: list_projects(w["id"]) for w in ws}
        diagram_counts: dict[str, int] = {}
        for w in ws:
            for p in projects[w["id"]]:
                diagram_counts[f"{w['id']}/{p['id']}"] = len(list_diagrams(w["id"], p["id"]))
        self._json(200, {
            "version": SERVER_VERSION,
            "uptime": time.time() - STARTED_AT,
            "data_dir": str(DATA),
            "workspaces": len(ws),
            "projects": sum(len(v) for v in projects.values()),
            "diagrams": sum(diagram_counts.values()),
            "per_project": diagram_counts,
            "standalone_diagrams": len(list_standalone()),
            "active_session": read_json(session_path()),
        })

    def _events(self, qs: dict[str, str]) -> None:
        since = int(qs.get("since", "0"))
        limit = int(qs.get("limit", "500"))
        events = BUS.tail(since, limit=limit)
        self._json(200, {
            "events": [e.__dict__ for e in events],
            "last_seq": events[-1].seq if events else since,
        })

    def _events_stream(self, _qs: dict[str, str]) -> None:
        self._sse()
        sub = BUS.subscribe()
        # Initial heartbeat so the client knows the stream is live.
        self._sse_send(json.dumps({"type": "hello", "version": SERVER_VERSION}, separators=(",", ":")))
        last_heartbeat = time.time()
        try:
            while self._sse_open:
                try:
                    event = sub.get(timeout=1.0)
                except queue.Empty:
                    if time.time() - last_heartbeat > 15.0:
                        self._sse_send(json.dumps({"type": "heartbeat", "ts": time.time()}, separators=(",", ":")))
                        last_heartbeat = time.time()
                    continue
                if event is None:
                    break
                self._sse_send(json.dumps({"type": "event", **event.__dict__}, separators=(",", ":")))
                last_heartbeat = time.time()
        finally:
            BUS.unsubscribe(sub)

    def _session_get(self, _qs: dict[str, str]) -> None:
        self._json(200, read_json(session_path()) or {})

    # ---------- workspace ----------

    def _workspace_list(self) -> None:
        self._json(200, {"workspaces": list_workspaces()})

    def _workspace_create(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace") or body.get("id")
        err = validate_id(wid, kind="workspace")
        if err:
            self._json(400, {"error": err})
            return
        wid = safe_id(wid)
        holder = body.get("actor") or f"agent-{uuid.uuid4().hex[:6]}"
        ok, blocker = LOCKS.acquire(f"workspace:{wid}", holder)
        if not ok:
            self._json(409, {"error": f"workspace locked by {blocker or 'another actor'}"})
            return
        try:
            root = workspace_dir(wid)
            root.mkdir(parents=True, exist_ok=True)
            meta = read_json(root / "workspace.json", {}) or {}
            meta.update({"id": wid, "title": body.get("title", meta.get("title", wid)), "updated_at": now()})
            meta.setdefault("created_at", now())
            write_json_atomic(root / "workspace.json", meta)
            # Auto-create inbox project for immediate use.
            inbox = project_dir(wid, "inbox")
            (inbox / "diagrams").mkdir(parents=True, exist_ok=True)
            if not (inbox / "project.json").exists():
                write_json_atomic(inbox / "project.json", {
                    "id": "inbox", "workspace": wid, "title": "inbox",
                    "created_at": now(), "updated_at": now(),
                })
            BUS.emit(Event(
                seq=BUS.next_seq(), ts=now(), kind="workspace.create",
                actor=holder, workspace=wid, project=None, diagram=None,
                summary=f"workspace '{wid}' created",
                data={"title": meta["title"]},
            ))
            self._json(200, {"ok": True, "workspace": meta})
        finally:
            LOCKS.release(f"workspace:{wid}", holder)

    def _workspace_rename(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace") or body.get("id")
        err = validate_id(wid, kind="workspace")
        if err:
            self._json(400, {"error": err})
            return
        new_title = body.get("title")
        if not isinstance(new_title, str) or not new_title.strip():
            self._json(400, {"error": "title must be a non-empty string"})
            return
        meta_path = workspace_dir(wid) / "workspace.json"
        if not meta_path.exists():
            self._json(404, {"error": f"workspace not found: {wid}"})
            return
        meta = read_json(meta_path, {}) or {}
        meta["title"] = new_title.strip()
        meta["updated_at"] = now()
        write_json_atomic(meta_path, meta)
        BUS.emit(Event(
            seq=BUS.next_seq(), ts=now(), kind="workspace.rename",
            actor=body.get("actor", "agent"), workspace=wid, project=None, diagram=None,
            summary=f"workspace '{wid}' renamed to '{new_title}'", data={},
        ))
        self._json(200, {"ok": True, "workspace": meta})

    def _workspace_delete(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace") or body.get("id")
        err = validate_id(wid, kind="workspace")
        if err:
            self._json(400, {"error": err})
            return
        root = workspace_dir(wid)
        if not root.exists():
            self._json(404, {"error": f"workspace not found: {wid}"})
            return
        import shutil
        shutil.rmtree(root)
        BUS.emit(Event(
            seq=BUS.next_seq(), ts=now(), kind="workspace.delete",
            actor=body.get("actor", "agent"), workspace=wid, project=None, diagram=None,
            summary=f"workspace '{wid}' deleted", data={},
        ))
        self._json(200, {"ok": True, "id": wid})

    # ---------- project ----------

    def _project_list(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace") or "default"
        err = validate_id(wid, kind="workspace")
        if err:
            self._json(400, {"error": err})
            return
        self._json(200, {"workspace": wid, "projects": list_projects(wid)})

    def _project_create(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace") or "default"
        pid = body.get("project") or body.get("id")
        if err := validate_id(wid, kind="workspace"):
            self._json(400, {"error": err})
            return
        if err := validate_id(pid, kind="project"):
            self._json(400, {"error": err})
            return
        wid = safe_id(wid)
        pid = safe_id(pid)
        holder = body.get("actor") or f"agent-{uuid.uuid4().hex[:6]}"
        ok, blocker = LOCKS.acquire(f"project:{wid}/{pid}", holder)
        if not ok:
            self._json(409, {"error": f"project locked by {blocker or 'another actor'}"})
            return
        try:
            diagrams_dir(wid, pid).mkdir(parents=True, exist_ok=True)
            meta_path = project_dir(wid, pid) / "project.json"
            meta = read_json(meta_path, {}) or {}
            meta.update({
                "id": pid, "workspace": wid,
                "title": body.get("title", meta.get("title", pid)),
                "updated_at": now(),
            })
            meta.setdefault("created_at", now())
            write_json_atomic(meta_path, meta)
            BUS.emit(Event(
                seq=BUS.next_seq(), ts=now(), kind="project.create",
                actor=holder, workspace=wid, project=pid, diagram=None,
                summary=f"project '{wid}/{pid}' created", data={"title": meta["title"]},
            ))
            self._json(200, {"ok": True, "project": meta})
        finally:
            LOCKS.release(f"project:{wid}/{pid}", holder)

    def _project_rename(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace") or "default"
        pid = body.get("project") or body.get("id")
        for name, val in (("workspace", wid), ("project", pid)):
            if err := validate_id(val, kind=name):
                self._json(400, {"error": err})
                return
        new_title = body.get("title")
        if not isinstance(new_title, str) or not new_title.strip():
            self._json(400, {"error": "title must be a non-empty string"})
            return
        meta_path = project_dir(wid, pid) / "project.json"
        if not meta_path.exists():
            self._json(404, {"error": f"project not found: {wid}/{pid}"})
            return
        meta = read_json(meta_path, {}) or {}
        meta["title"] = new_title.strip()
        meta["updated_at"] = now()
        write_json_atomic(meta_path, meta)
        BUS.emit(Event(
            seq=BUS.next_seq(), ts=now(), kind="project.rename",
            actor=body.get("actor", "agent"), workspace=wid, project=pid, diagram=None,
            summary=f"project '{wid}/{pid}' renamed to '{new_title}'", data={},
        ))
        self._json(200, {"ok": True, "project": meta})

    def _project_delete(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace") or "default"
        pid = body.get("project") or body.get("id")
        for name, val in (("workspace", wid), ("project", pid)):
            if err := validate_id(val, kind=name):
                self._json(400, {"error": err})
                return
        root = project_dir(wid, pid)
        if not root.exists():
            self._json(404, {"error": f"project not found: {wid}/{pid}"})
            return
        import shutil
        shutil.rmtree(root)
        BUS.emit(Event(
            seq=BUS.next_seq(), ts=now(), kind="project.delete",
            actor=body.get("actor", "agent"), workspace=wid, project=pid, diagram=None,
            summary=f"project '{wid}/{pid}' deleted", data={},
        ))
        self._json(200, {"ok": True, "workspace": wid, "project": pid})

    def _project_copy(self, body: dict[str, Any]) -> None:
        # Back-compat alias of /diagram/put (legacy TUI used this).
        return self._diagram_put(body)

    def _project_diagrams(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace") or "default"
        pid = body.get("project") or "inbox"
        for name, val in (("workspace", wid), ("project", pid)):
            if err := validate_id(val, kind=name):
                self._json(400, {"error": err})
                return
        self._json(200, {
            "workspace": wid,
            "project": pid,
            "diagrams": list_diagrams(wid, pid),
        })

    # ---------- diagram ----------

    def _diagram_list(self, body: dict[str, Any]) -> None:
        wid = body.get("workspace")
        pid = body.get("project")
        if wid and pid:
            for name, val in (("workspace", wid), ("project", pid)):
                if err := validate_id(val, kind=name):
                    self._json(400, {"error": err})
                    return
            self._json(200, {
                "scope": "project", "workspace": wid, "project": pid,
                "diagrams": list_diagrams(wid, pid),
            })
        else:
            self._json(200, {"scope": "standalone", "diagrams": list_standalone()})

    def _diagram_get(self, body: dict[str, Any]) -> None:
        did = body.get("id") or body.get("diagram")
        if err := validate_id(did, kind="diagram"):
            self._json(400, {"error": err})
            return
        did = safe_id(did)
        wid = body.get("workspace")
        pid = body.get("project")
        if wid and pid:
            p = diagrams_dir(wid, pid) / f"{did}{FILE_EXT}"
        else:
            p = standalone_dir() / f"{did}{FILE_EXT}"
        if not p.exists():
            self._json(404, {"error": f"diagram not found: {did}"})
            return
        self._json(200, {"id": did, "document": read_json(p), "path": str(p)})

    def _diagram_put(self, body: dict[str, Any]) -> None:
        did = body.get("id") or body.get("diagram")
        doc = body.get("document") or body.get("data")
        if err := validate_id(did, kind="diagram"):
            self._json(400, {"error": err})
            return
        if doc is None and body.get("source_path"):
            sp = Path(body["source_path"])
            if not sp.exists():
                self._json(404, {"error": f"source_path not found: {body['source_path']}"})
                return
            doc = read_json(sp)
        if err := _validate(doc):
            self._json(400, {"error": err})
            return
        did = safe_id(did) or safe_id(doc.get("id"), fallback=f"diagram-{uuid.uuid4().hex[:8]}")
        # Ensure doc id matches if absent.
        doc.setdefault("id", did)
        wid = body.get("workspace")
        pid = body.get("project")
        if wid and pid:
            for name, val in (("workspace", wid), ("project", pid)):
                if err := validate_id(val, kind=name):
                    self._json(400, {"error": err})
                    return
            wid = safe_id(wid)
            pid = safe_id(pid)
            diagrams_dir(wid, pid).mkdir(parents=True, exist_ok=True)
            p = diagrams_dir(wid, pid) / f"{did}{FILE_EXT}"
        else:
            standalone_dir().mkdir(parents=True, exist_ok=True)
            p = standalone_dir() / f"{did}{FILE_EXT}"
        holder = body.get("actor") or f"agent-{uuid.uuid4().hex[:6]}"
        ok, blocker = LOCKS.acquire(f"diagram:{p}", holder)
        if not ok:
            self._json(409, {"error": f"diagram locked by {blocker or 'another actor'}"})
            return
        try:
            write_json_atomic(p, doc)
            meta = {
                "id": did,
                "title": doc.get("title", did),
                "kind": doc.get("kind", "unknown"),
                "actor": holder,
                "updated_at": now(),
            }
            if wid and pid:
                meta["workspace"] = wid
                meta["project"] = pid
            write_json_atomic(p.with_suffix(".meta.json"), meta)
            BUS.emit(Event(
                seq=BUS.next_seq(), ts=now(), kind="diagram.put",
                actor=holder, workspace=wid, project=pid, diagram=did,
                summary=f"diagram '{did}' saved" + (f" to {wid}/{pid}" if wid and pid else ""),
                data={"title": meta["title"], "kind": meta["kind"]},
            ))
            self._json(200, {"ok": True, "id": did, "path": str(p), "meta": meta})
        finally:
            LOCKS.release(f"diagram:{p}", holder)

    def _diagram_delete(self, body: dict[str, Any]) -> None:
        did = body.get("id") or body.get("diagram")
        if err := validate_id(did, kind="diagram"):
            self._json(400, {"error": err})
            return
        wid = body.get("workspace")
        pid = body.get("project")
        if wid and pid:
            for name, val in (("workspace", wid), ("project", pid)):
                if err := validate_id(val, kind=name):
                    self._json(400, {"error": err})
                    return
            p = diagrams_dir(wid, pid) / f"{safe_id(did)}{FILE_EXT}"
            scope = f"{wid}/{pid}"
        else:
            p = standalone_dir() / f"{safe_id(did)}{FILE_EXT}"
            scope = "standalone"
        if not p.exists():
            self._json(404, {"error": f"diagram not found: {did}"})
            return
        p.unlink()
        with contextlib.suppress(FileNotFoundError):
            p.with_suffix(".meta.json").unlink()
        BUS.emit(Event(
            seq=BUS.next_seq(), ts=now(), kind="diagram.delete",
            actor=body.get("actor", "agent"), workspace=wid, project=pid, diagram=did,
            summary=f"diagram '{did}' deleted from {scope}", data={},
        ))
        self._json(200, {"ok": True, "id": did, "scope": scope})

    def _diagram_render(self, body: dict[str, Any]) -> None:
        doc = body.get("document") or body.get("data")
        if err := _validate(doc):
            self._json(400, {"error": err})
            return
        result = render_diagram(
            doc,
            int(body.get("width", 120)),
            int(body.get("height", 40)),
            str(body.get("theme", "neon")),
            str(body.get("color", "never")),
        )
        if result.get("error"):
            self._json(503, result)
            return
        self._json(200, result)

    # ---------- patches ----------

    def _patch_apply(self, body: dict[str, Any]) -> None:
        did = body.get("id") or body.get("diagram")
        if err := validate_id(did, kind="diagram"):
            self._json(400, {"error": err})
            return
        ops = body.get("ops") or body.get("patch") or body.get("operations")
        if not isinstance(ops, list):
            self._json(400, {"error": "ops must be a list"})
            return
        wid = body.get("workspace")
        pid = body.get("project")
        if wid and pid:
            for name, val in (("workspace", wid), ("project", pid)):
                if err := validate_id(val, kind=name):
                    self._json(400, {"error": err})
                    return
            p = diagrams_dir(wid, pid) / f"{safe_id(did)}{FILE_EXT}"
        else:
            p = standalone_dir() / f"{safe_id(did)}{FILE_EXT}"
        if not p.exists():
            self._json(404, {"error": f"diagram not found: {did}"})
            return
        doc = read_json(p, {})
        if not isinstance(doc, dict):
            self._json(500, {"error": "stored document is not an object"})
            return
        try:
            doc = apply_patch(doc, ops)
        except ValueError as e:
            self._json(400, {"error": f"patch failed: {e}"})
            return
        if err := _validate(doc):
            self._json(400, {"error": f"post-patch invalid: {err}"})
            return
        write_json_atomic(p, doc)
        meta = {
            "id": did, "title": doc.get("title", did), "kind": doc.get("kind", "unknown"),
            "actor": body.get("actor", "agent"), "updated_at": now(),
        }
        if wid and pid:
            meta["workspace"] = wid
            meta["project"] = pid
        write_json_atomic(p.with_suffix(".meta.json"), meta)
        BUS.emit(Event(
            seq=BUS.next_seq(), ts=now(), kind="diagram.patch",
            actor=body.get("actor", "agent"), workspace=wid, project=pid, diagram=did,
            summary=f"patched '{did}' ({len(ops)} ops)", data={"ops": len(ops)},
        ))
        self._json(200, {"ok": True, "id": did, "path": str(p), "ops": len(ops)})

    # ---------- session pointer ----------

    def _session_set(self, body: dict[str, Any]) -> None:
        cur = read_json(session_path(), {}) or {}
        for k in ("workspace", "project", "diagram"):
            if k in body:
                cur[k] = body[k] or None
        cur["updated_at"] = now()
        cur["actor"] = body.get("actor", cur.get("actor", "agent"))
        write_json_atomic(session_path(), cur)
        BUS.emit(Event(
            seq=BUS.next_seq(), ts=now(), kind="session.active",
            actor=cur["actor"],
            workspace=cur.get("workspace"), project=cur.get("project"),
            diagram=cur.get("diagram"),
            summary="active session set", data=cur,
        ))
        self._json(200, cur)

    # ---------- locks ----------

    def _lock_acquire(self, body: dict[str, Any]) -> None:
        resource = body.get("resource")
        holder = body.get("holder") or f"agent-{uuid.uuid4().hex[:6]}"
        if not isinstance(resource, str) or not resource:
            self._json(400, {"error": "resource required"})
            return
        ok, blocker = LOCKS.acquire(resource, holder)
        if not ok:
            self._json(409, {"ok": False, "holder": blocker})
            return
        self._json(200, {"ok": True, "resource": resource, "holder": holder})

    def _lock_release(self, body: dict[str, Any]) -> None:
        resource = body.get("resource")
        holder = body.get("holder")
        if not isinstance(resource, str) or not resource:
            self._json(400, {"error": "resource required"})
            return
        LOCKS.release(resource, holder or "")
        self._json(200, {"ok": True, "resource": resource})


# ---------------------------------------------------------------------------
# Patch application (domain-aware; supports add/update/remove primitives)
# ---------------------------------------------------------------------------


def apply_patch(doc: dict[str, Any], ops: list[dict[str, Any]]) -> dict[str, Any]:
    """Apply a list of patch operations to a diagram document.

    Operations supported (kept minimal on purpose — the renderer does not
    need the JSON-Patch surface):

      {"op": "node.add",    "node": {...}}
      {"op": "node.update", "id": "...", "patch": {...}}
      {"op": "node.remove", "id": "..."}
      {"op": "edge.add",    "edge": {...}}
      {"op": "edge.update", "id": "...", "patch": {...}}
      {"op": "edge.remove", "id": "..."}
      {"op": "annotation.add", "annotation": {...}}
      {"op": "annotation.remove", "id": "..."}
      {"op": "group.add", "group": {...}}
      {"op": "group.remove", "id": "..."}
      {"op": "meta.set", "patch": {...}}
    """
    doc = json.loads(json.dumps(doc))  # deep copy without importing copy
    nodes = {n["id"]: n for n in doc.get("nodes", [])}
    edges = {e["id"]: e for e in doc.get("edges", [])}
    annotations = {a["id"]: a for a in doc.get("annotations", [])}
    groups = {g["id"]: g for g in doc.get("groups", [])}

    for op in ops:
        kind = op.get("op")
        if kind == "node.add":
            node = op["node"]
            if "id" not in node:
                raise ValueError("node.add requires node.id")
            nodes[node["id"]] = node
        elif kind == "node.update":
            node = nodes.get(op["id"])
            if not node:
                raise ValueError(f"node.update: unknown id {op['id']}")
            node.update(op.get("patch", {}))
        elif kind == "node.remove":
            target = op["id"]
            nodes.pop(target, None)
            # Garbage-collect dangling edges.
            for eid in [eid for eid, e in edges.items()
                        if (e.get("source") or {}).get("node") == target
                        or (e.get("target") or {}).get("node") == target]:
                edges.pop(eid, None)
        elif kind == "edge.add":
            edge = op["edge"]
            if "id" not in edge:
                raise ValueError("edge.add requires edge.id")
            edges[edge["id"]] = edge
        elif kind == "edge.update":
            edge = edges.get(op["id"])
            if not edge:
                raise ValueError(f"edge.update: unknown id {op['id']}")
            edge.update(op.get("patch", {}))
        elif kind == "edge.remove":
            edges.pop(op["id"], None)
        elif kind == "annotation.add":
            ann = op["annotation"]
            if "id" not in ann:
                raise ValueError("annotation.add requires annotation.id")
            annotations[ann["id"]] = ann
        elif kind == "annotation.remove":
            annotations.pop(op["id"], None)
        elif kind == "group.add":
            grp = op["group"]
            if "id" not in grp:
                raise ValueError("group.add requires group.id")
            groups[grp["id"]] = grp
        elif kind == "group.remove":
            groups.pop(op["id"], None)
        elif kind == "meta.set":
            for k, v in (op.get("patch") or {}).items():
                if k in ("nodes", "edges", "annotations", "groups", "schema_version"):
                    raise ValueError(f"meta.set: cannot overwrite {k!r}")
                doc[k] = v
        else:
            raise ValueError(f"unknown op: {kind!r}")

    doc["nodes"] = list(nodes.values())
    doc["edges"] = list(edges.values())
    if annotations:
        doc["annotations"] = list(annotations.values())
    if groups:
        doc["groups"] = list(groups.values())
    return doc


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def serve(host: str, port: int) -> int:
    DATA.mkdir(parents=True, exist_ok=True)
    (DATA / "workspaces").mkdir(parents=True, exist_ok=True)
    (DATA / "diagrams").mkdir(parents=True, exist_ok=True)
    # Touch events file so subscribers never get a 404.
    events_path().touch(exist_ok=True)
    server = ThreadingHTTPServer((host, port), Handler)
    print(f"diagram_workspace {SERVER_VERSION} listening on http://{host}:{port} (data={DATA})", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="diagram_workspace relic server")
    ap.add_argument("--host", default=os.environ.get("DIAGRAM_WORKSPACE_HOST", "0.0.0.0"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("DIAGRAM_WORKSPACE_PORT", "8127")))
    args = ap.parse_args(argv)
    return serve(args.host, args.port)


if __name__ == "__main__":
    raise SystemExit(main())
