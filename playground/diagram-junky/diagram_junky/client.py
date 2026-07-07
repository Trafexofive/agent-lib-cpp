"""HTTP + SSE client for the diagram_workspace relic.

Two pieces:

- ``WorkspaceClient``: synchronous, thread-safe JSON-RPC client. Use it
  from a TUI's main thread or from a tool wrapper. Methods map 1:1 to
  the relic endpoints.

- ``EventStream``: a background thread that consumes ``/events/stream``
  (server-sent events) and pushes decoded events into a thread-safe
  queue the main thread can drain. Auto-reconnects with exponential
  backoff if the server drops the connection. Falls back to polling
  ``/events?since=N`` if the stream endpoint is unavailable.

Both are deliberately stdlib-only so the diagram-junky playground
stays dependency-free.
"""

from __future__ import annotations

import json
import os
import queue
import re
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Callable, Iterator


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class WorkspaceError(RuntimeError):
    """Raised when a workspace API call fails. Carries status + body."""

    def __init__(self, message: str, *, status: int | None = None, body: Any = None):
        super().__init__(message)
        self.status = status
        self.body = body


# ---------------------------------------------------------------------------
# Workspace client
# ---------------------------------------------------------------------------


class WorkspaceClient:
    """Synchronous JSON client for the diagram_workspace relic."""

    def __init__(
        self,
        base_url: str = "http://localhost:8127",
        *,
        timeout: float = 5.0,
        actor: str | None = None,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.actor = actor
        self._lock = threading.Lock()

    # ---------- low-level ----------

    def _request(self, endpoint: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        url = f"{self.base_url}{endpoint}"
        data = json.dumps(payload or {}).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=data,
            headers={"Content-Type": "application/json", "Accept": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            try:
                body = json.loads(e.read().decode("utf-8"))
            except Exception:
                body = None
            raise WorkspaceError(f"{endpoint}: HTTP {e.code}", status=e.code, body=body) from e
        except urllib.error.URLError as e:
            raise WorkspaceError(f"{endpoint}: {e.reason}") from e
        try:
            result = json.loads(raw)
        except json.JSONDecodeError as e:
            raise WorkspaceError(f"{endpoint}: invalid json: {e}") from e
        if isinstance(result, dict) and "error" in result and "ok" not in result:
            raise WorkspaceError(f"{endpoint}: {result['error']}", body=result)
        return result

    def _get(self, endpoint: str) -> dict[str, Any]:
        url = f"{self.base_url}{endpoint}"
        req = urllib.request.Request(url, headers={"Accept": "application/json"}, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                body = json.loads(e.read().decode("utf-8"))
            except Exception:
                body = None
            raise WorkspaceError(f"{endpoint}: HTTP {e.code}", status=e.code, body=body) from e
        except urllib.error.URLError as e:
            raise WorkspaceError(f"{endpoint}: {e.reason}") from e

    # ---------- session actor ----------

    def _with_actor(self, payload: dict[str, Any] | None) -> dict[str, Any]:
        payload = dict(payload or {})
        if self.actor and "actor" not in payload:
            payload["actor"] = self.actor
        return payload

    # ---------- workspace ----------

    def health(self) -> dict[str, Any]:
        return self._get("/health")

    def version(self) -> dict[str, Any]:
        return self._get("/version")

    def info(self) -> dict[str, Any]:
        return self._get("/info")

    def list_workspaces(self) -> list[dict[str, Any]]:
        return self._request("/workspace/list").get("workspaces", [])

    def create_workspace(self, *, workspace: str, title: str | None = None) -> dict[str, Any]:
        return self._request("/workspace/create", self._with_actor({"workspace": workspace, "title": title or workspace}))

    def rename_workspace(self, *, workspace: str, title: str) -> dict[str, Any]:
        return self._request("/workspace/rename", self._with_actor({"workspace": workspace, "title": title}))

    def delete_workspace(self, *, workspace: str) -> dict[str, Any]:
        return self._request("/workspace/delete", self._with_actor({"workspace": workspace}))

    # ---------- project ----------

    def list_projects(self, *, workspace: str) -> list[dict[str, Any]]:
        return self._request("/project/list", {"workspace": workspace}).get("projects", [])

    def create_project(self, *, workspace: str, project: str, title: str | None = None) -> dict[str, Any]:
        return self._request(
            "/project/create",
            self._with_actor({"workspace": workspace, "project": project, "title": title or project}),
        )

    def rename_project(self, *, workspace: str, project: str, title: str) -> dict[str, Any]:
        return self._request("/project/rename", self._with_actor({"workspace": workspace, "project": project, "title": title}))

    def delete_project(self, *, workspace: str, project: str) -> dict[str, Any]:
        return self._request("/project/delete", self._with_actor({"workspace": workspace, "project": project}))

    def list_project_diagrams(self, *, workspace: str, project: str) -> list[dict[str, Any]]:
        return self._request("/project/diagrams", {"workspace": workspace, "project": project}).get("diagrams", [])

    # ---------- diagram ----------

    def list_diagrams(self, *, workspace: str | None = None, project: str | None = None) -> list[dict[str, Any]]:
        if workspace and project:
            return self._request("/diagram/list", {"workspace": workspace, "project": project}).get("diagrams", [])
        return self._request("/diagram/list", {}).get("diagrams", [])

    def get_diagram(
        self,
        *,
        id: str,
        workspace: str | None = None,
        project: str | None = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"id": id}
        if workspace:
            body["workspace"] = workspace
        if project:
            body["project"] = project
        return self._request("/diagram/get", body)

    def put_diagram(
        self,
        *,
        id: str,
        document: dict[str, Any],
        workspace: str | None = None,
        project: str | None = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"id": id, "document": document}
        if workspace:
            body["workspace"] = workspace
        if project:
            body["project"] = project
        return self._request("/diagram/put", self._with_actor(body))

    def delete_diagram(
        self,
        *,
        id: str,
        workspace: str | None = None,
        project: str | None = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"id": id}
        if workspace:
            body["workspace"] = workspace
        if project:
            body["project"] = project
        return self._request("/diagram/delete", self._with_actor(body))

    def render_diagram(
        self,
        *,
        document: dict[str, Any],
        width: int = 120,
        height: int = 40,
        theme: str = "neon",
        color: str = "never",
    ) -> dict[str, Any]:
        return self._request(
            "/diagram/render",
            {"document": document, "width": width, "height": height, "theme": theme, "color": color},
        )

    def apply_patch(
        self,
        *,
        id: str,
        ops: list[dict[str, Any]],
        workspace: str | None = None,
        project: str | None = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"id": id, "ops": ops}
        if workspace:
            body["workspace"] = workspace
        if project:
            body["project"] = project
        return self._request("/patch/apply", self._with_actor(body))

    # ---------- events + session ----------

    def events(self, *, since: int = 0, limit: int = 200) -> dict[str, Any]:
        return self._get(f"/events?since={int(since)}&limit={int(limit)}")

    def session_active(self) -> dict[str, Any]:
        return self._get("/session/active")

    def set_session_active(
        self,
        *,
        workspace: str | None = None,
        project: str | None = None,
        diagram: str | None = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {}
        if workspace is not None:
            body["workspace"] = workspace or None
        if project is not None:
            body["project"] = project or None
        if diagram is not None:
            body["diagram"] = diagram or None
        return self._request("/session/active", self._with_actor(body))

    # ---------- context manager ----------

    def __enter__(self) -> "WorkspaceClient":
        return self

    def __exit__(self, *_: object) -> None:
        return


# ---------------------------------------------------------------------------
# Event stream (SSE subscriber with polling fallback)
# ---------------------------------------------------------------------------


@dataclass
class StreamEvent:
    """Decoded event from the relic's event stream."""

    type: str  # "event" | "hello" | "heartbeat"
    raw: dict[str, Any] = field(default_factory=dict)
    received_at: float = field(default_factory=time.time)

    @property
    def kind(self) -> str | None:
        return self.raw.get("kind")

    @property
    def seq(self) -> int:
        return int(self.raw.get("seq", 0))

    @property
    def summary(self) -> str:
        return self.raw.get("summary", "")


class EventStream:
    """Background consumer for ``/events/stream`` (SSE) with polling fallback.

    Connect once, then drain ``events()`` from the main thread. Reconnects
    automatically with exponential backoff if the stream drops. If the
    server doesn't support SSE, the stream silently falls back to
    polling ``/events?since=last_seq`` every second.
    """

    def __init__(
        self,
        client: WorkspaceClient,
        *,
        on_event: Callable[[StreamEvent], None] | None = None,
        on_state: Callable[[str], None] | None = None,
        reconnect_initial: float = 0.5,
        reconnect_max: float = 8.0,
    ) -> None:
        self.client = client
        self.on_event = on_event
        self.on_state = on_state
        self.reconnect_initial = reconnect_initial
        self.reconnect_max = reconnect_max
        self._events: queue.Queue[StreamEvent | None] = queue.Queue()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_seq = 0
        self._state = "idle"

    @property
    def state(self) -> str:
        return self._state

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="dj-event-stream", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._events.put(None)

    def events(self, *, block: bool = True, timeout: float = 0.2) -> list[StreamEvent]:
        """Drain pending events. Returns [] on timeout."""
        out: list[StreamEvent] = []
        try:
            while True:
                evt = self._events.get(block=block, timeout=timeout if out else timeout)
                if evt is None:
                    break
                out.append(evt)
        except queue.Empty:
            pass
        return out

    def _set_state(self, state: str) -> None:
        self._state = state
        if self.on_state:
            try:
                self.on_state(state)
            except Exception:
                pass

    def _push(self, evt: StreamEvent) -> None:
        try:
            self._events.put_nowait(evt)
        except queue.Full:
            pass
        if self.on_event:
            try:
                self.on_event(evt)
            except Exception:
                pass

    def _run(self) -> None:
        backoff = self.reconnect_initial
        while not self._stop.is_set():
            try:
                self._consume_sse()
                backoff = self.reconnect_initial
            except Exception as e:  # noqa: BLE001
                if self._stop.is_set():
                    break
                self._set_state(f"disconnected ({e!s}); retrying in {backoff:.1f}s")
                if self._stop.wait(backoff):
                    break
                backoff = min(self.reconnect_max, backoff * 2)
        self._set_state("stopped")

    def _consume_sse(self) -> None:
        self._set_state("connecting (sse)")
        url = f"{self.client.base_url}/events/stream"
        req = urllib.request.Request(url, headers={"Accept": "text/event-stream"})
        with urllib.request.urlopen(req, timeout=None) as resp:
            if "text/event-stream" not in (resp.headers.get("Content-Type") or ""):
                # Server didn't return SSE — fall back to polling.
                self._set_state("connected (polling)")
                self._consume_polling()
                return
            self._set_state("connected (sse)")
            self._read_sse_chunks(resp)

    def _read_sse_chunks(self, resp: Any) -> None:
        decoder_chunk = b""
        for raw in resp:
            if self._stop.is_set():
                break
            chunk = decoder_chunk + raw
            lines = chunk.split(b"\n")
            decoder_chunk = lines.pop()  # may be incomplete
            for line_bytes in lines:
                line = line_bytes.decode("utf-8", "replace").rstrip("\r")
                if not line or not line.startswith("data:"):
                    continue
                payload = line[len("data:"):].strip()
                if not payload:
                    continue
                try:
                    raw_obj = json.loads(payload)
                except json.JSONDecodeError:
                    continue
                evt = StreamEvent(type=raw_obj.get("type", "event"), raw=raw_obj)
                self._push(evt)
                if evt.type == "event":
                    seq = int(raw_obj.get("seq", 0))
                    if seq > self._last_seq:
                        self._last_seq = seq

    def _consume_polling(self) -> None:
        while not self._stop.is_set():
            try:
                resp = self.client.events(since=self._last_seq, limit=100)
                for raw in resp.get("events", []):
                    seq = int(raw.get("seq", 0))
                    if seq <= self._last_seq:
                        continue
                    self._last_seq = seq
                    self._push(StreamEvent(type="event", raw=raw))
            except Exception as e:  # noqa: BLE001
                self._set_state(f"poll error: {e!s}")
            if self._stop.wait(1.0):
                break


# ---------------------------------------------------------------------------
# File-watch helper (for local mode)
# ---------------------------------------------------------------------------


class FileWatch:
    """Tiny mtime-based file watcher.

    Polls a single file's mtime in a background thread and emits a
    StreamEvent-like object when it changes. Used in local mode so the
    TUI can auto-reload the current diagram when it is edited on disk.
    """

    @dataclass
    class Change:
        path: str
        mtime: float

    def __init__(self) -> None:
        self._changes: queue.Queue[FileWatch.Change] = queue.Queue()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._path: str | None = None
        self._last_mtime: float = 0.0

    def watch(self, path: str) -> None:
        self._path = path
        try:
            self._last_mtime = path and os.path.getmtime(path) or 0.0
        except OSError:
            self._last_mtime = 0.0
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="dj-file-watch", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=0.5)
        self._thread = None

    def changes(self, *, timeout: float = 0.2) -> list[Change]:
        out: list[Change] = []
        try:
            while True:
                out.append(self._changes.get(timeout=timeout if not out else 0.05))
        except queue.Empty:
            pass
        return out

    def _run(self) -> None:
        while not self._stop.is_set():
            if self._path:
                try:
                    mt = os.path.getmtime(self._path)
                except OSError:
                    mt = 0.0
                if mt and mt != self._last_mtime:
                    self._last_mtime = mt
                    self._changes.put(self.Change(path=self._path, mtime=mt))
            if self._stop.wait(0.4):
                break
