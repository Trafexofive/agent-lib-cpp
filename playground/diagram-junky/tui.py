#!/usr/bin/env python3
"""Raw ANSI diagram-junky TUI prototype — local + client modes.

Two connection modes:

  local   (default): opens .diagram.json files from disk. No server.
  client  (--client): attaches to a diagram_workspace relic over HTTP.
                       Subscribes to /events/stream, follows the active
                       session pointer, shows a live activity feed, and
                       auto-reloads the current diagram when it changes.

UX shape (still glow/dash-style, no curses, no mouse):

  header (2 rows) -> dashboard / canvas / chat (body) -> footer (2 rows)
  dashboard: documents on the left, preview on the right
  canvas:    diagram with optional sidebar + crosshair
  chat:      harness pet transcript + input
  overlays:  help (?) · command (:) · search (/) · prompt (text input)
  pet:       5x5 tamagotchi at bottom-right when canvas is wide enough
  zoom:      relative to canvas center — the thing under the crosshair
             stays put when you press +/-.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import shutil
import subprocess
import sys
import termios
import threading
import time
import tty
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from urllib import error as urlerror
from urllib import request as urlrequest

from diagram_junky.client import (
    EventStream,
    FileWatch,
    StreamEvent,
    WorkspaceClient,
    WorkspaceError,
)
from diagram_junky.rendering import EXAMPLES_DIR, THEMES, Renderer, diagram_bounds, load_doc

ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = ROOT.parents[1]
DEFAULT_STATE = Path.home() / ".cache" / "diagram-junky" / "tui-state.json"
THEME_ORDER = ["default", "mono", "neon"]
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")

RESET = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"
CYAN = "\033[36m"
MAGENTA = "\033[35m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
RED = "\033[31m"
BLUE = "\033[34m"
BRIGHT_CYAN = "\033[96m"
BRIGHT_MAGENTA = "\033[95m"

# Activity feed cap. Keeps memory bounded and the sidebar readable.
FEED_MAX = 64
# SSE reconnect backoff bounds.
SSE_BACKOFF_INITIAL = 0.5
SSE_BACKOFF_MAX = 8.0


# ---------------------------------------------------------------------------
# View + small helpers
# ---------------------------------------------------------------------------


@dataclass
class View:
    x: float = 0.0
    y: float = 0.0
    zoom: float = 1.0


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


def visible_len(s: str) -> int:
    return len(strip_ansi(s))


def fit_ansi(s: str, width: int) -> str:
    """Clip/pad a possibly-ANSI string by visible width."""
    if width <= 0:
        return ""
    out = ""
    visible = 0
    i = 0
    while i < len(s) and visible < width:
        if s[i] == "\033":
            m = ANSI_RE.match(s, i)
            if m:
                out += m.group(0)
                i = m.end()
                continue
        out += s[i]
        visible += 1
        i += 1
    if "\033[" in out and not out.endswith(RESET):
        out += RESET
    return out + (" " * max(0, width - visible))


def plain_fit(s: str, width: int) -> str:
    clean = strip_ansi(s)
    if len(clean) > width:
        clean = clean[: max(0, width - 1)] + "…"
    return clean + " " * max(0, width - len(clean))


def style(s: str, enabled: bool, *codes: str) -> str:
    if not enabled:
        return s
    return "".join(codes) + s + RESET


def rule(title: str, width: int, *, color: bool = False, active: bool = False) -> str:
    width = max(4, width)
    pen = CYAN if active else DIM
    label = f" {title.strip()} " if title.strip() else ""
    line = label + "─" * max(0, width - visible_len(label))
    line = fit_ansi(line, width)
    return style(line, color, pen)


def panel(title: str, width: int, height: int, body: list[str], *, color: bool = False, active: bool = False) -> list[str]:
    width = max(4, width)
    height = max(1, height)
    lines = [rule(title, width, color=color, active=active)]
    if height > 1:
        lines.append(" " * width)
    for i in range(max(0, height - len(lines))):
        content = body[i] if i < len(body) else ""
        lines.append(fit_ansi(content, width))
    return lines[:height]


class RawMode:
    def __enter__(self) -> "RawMode":
        self.fd = sys.stdin.fileno()
        self.old = termios.tcgetattr(self.fd)
        tty.setcbreak(self.fd)
        return self

    def __exit__(self, *_: object) -> None:
        termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old)


# ---------------------------------------------------------------------------
# Activity feed (server events + local actions)
# ---------------------------------------------------------------------------


@dataclass
class FeedItem:
    ts: float
    text: str
    kind: str = "info"  # info | create | update | delete | patch | session | error | local
    source: str = "local"


def short_ago(ts: float, now: float) -> str:
    dt = max(0.0, now - ts)
    if dt < 1.0:
        return "now"
    if dt < 60:
        return f"{int(dt)}s"
    if dt < 3600:
        return f"{int(dt // 60)}m"
    return f"{int(dt // 3600)}h"


# ---------------------------------------------------------------------------
# Diagram TUI
# ---------------------------------------------------------------------------


class DiagramTui:
    """The TUI. Single class; modes (dashboard/canvas/chat) and overlays
    (help/command/search/prompt) share a common draw pipeline."""

    def __init__(
        self,
        path: Path | None,
        example: str | None,
        state_path: Path,
        color: bool,
        *,
        client_mode: bool = False,
        server_url: str = "http://localhost:8127",
        provider: str = "deepseek",
        model: str = "deepseek-v4-pro",
        model_timeout: int = 120,
    ):
        self.state_path = state_path
        self.color = color
        self.client_mode = client_mode
        self.server_url = server_url.rstrip("/")
        self.provider = provider
        self.model = model
        self.model_timeout = model_timeout

        # Workspace state (used in both modes for navigation).
        self.workspaces: list[dict[str, Any]] = []
        self.projects: list[dict[str, Any]] = []
        self.project_diagrams: list[dict[str, Any]] = []
        self.current_workspace = "default"
        self.current_project = "inbox"
        self.diagram_index = 0

        # Pet/chat state.
        self.pet_state = "idle"
        self.pet_done_at = 0.0
        self.model_output = ""
        self.chat_input = ""
        self.chat_history: list[tuple[str, str]] = []
        self.model_thread: threading.Thread | None = None

        # Examples.
        self.examples = sorted(EXAMPLES_DIR.glob("*.diagram.json"))

        # View + theme.
        self.path = self._resolve_path(path, example)
        self.doc = load_doc(self.path, safe=True)
        self.view = View()
        self.theme = "neon"
        self.legend = True
        self.ports = False
        self.sidebar_visible = False
        self.crosshair_visible = True
        self.animating = False

        # Mode: dashboard first, canvas if a specific file/example given.
        self.mode = "canvas" if path or example else "dashboard"
        self.overlay: str | None = None  # help | command | search | prompt
        self.prompt_label = ""
        self.prompt_buffer = ""
        self.prompt_choices: list[tuple[str, str]] = []  # (key, label) for chooser mode
        self.search_query = ""
        self.search_index = 0

        # Activity feed (client mode uses it; local mode uses it too for
        # the user's own actions, so the two modes look the same).
        self.feed: deque[FeedItem] = deque(maxlen=FEED_MAX)
        self.last_seq = 0
        self.follow_active = True  # when client mode + on, auto-open active
        self.live_reload = True   # auto-reload the current diagram on change

        # Connection state.
        self.conn_state = "local"
        self.conn_error: str | None = None
        self.client: WorkspaceClient | None = None
        self.stream: EventStream | None = None
        self.watcher: FileWatch | None = None
        self.watched_diagram_path: str | None = None

        # Misc UI state.
        self.started_at = time.monotonic()
        self.message = ""
        self.message_until = 0.0
        self.last_health_check = 0.0
        self.server_health: dict[str, Any] | None = None

        self._load_state_if_relevant(path, example)
        self._sync_example_index()
        if self.client_mode:
            self._start_client()
        else:
            self._start_local_watch()

    # ---------- path/example resolution ----------

    def _resolve_path(self, path: Path | None, example: str | None) -> Path:
        if example:
            p = EXAMPLES_DIR / (example if example.endswith(".json") else f"{example}.diagram.json")
            if not p.exists():
                raise FileNotFoundError(f"unknown example: {example}")
            return p
        if path:
            return path
        return self.examples[0] if self.examples else ROOT / "examples" / "minimal-flow.diagram.json"

    def _sync_example_index(self) -> None:
        try:
            self.example_index = self.examples.index(self.path)
        except ValueError:
            self.example_index = 0

    # ---------- state persistence ----------

    def _load_state_if_relevant(self, path: Path | None, example: str | None) -> None:
        if path or example or not self.state_path.exists():
            return
        try:
            raw = json.loads(self.state_path.read_text())
            saved_path = Path(raw.get("path", ""))
            if saved_path.exists():
                self.path = saved_path
                self.doc = load_doc(self.path, safe=True)
            self.view = View(float(raw.get("x", 0)), float(raw.get("y", 0)), float(raw.get("zoom", 1)))
            theme = raw.get("theme", self.theme)
            self.theme = theme if theme in THEMES else self.theme
            self.legend = bool(raw.get("legend", self.legend))
            self.ports = bool(raw.get("ports", self.ports))
            self.sidebar_visible = bool(raw.get("sidebar_visible", self.sidebar_visible))
            self.crosshair_visible = bool(raw.get("crosshair_visible", self.crosshair_visible))
            self.current_workspace = raw.get("current_workspace", self.current_workspace)
            self.current_project = raw.get("current_project", self.current_project)
            self.follow_active = bool(raw.get("follow_active", self.follow_active))
            self.live_reload = bool(raw.get("live_reload", self.live_reload))
            self.set_message(f"loaded state from {self.state_path}", ttl=1.2)
        except Exception as e:
            self.set_message(f"state load skipped: {e}", ttl=1.5)

    def save_state(self) -> None:
        payload: dict[str, Any] = {
            "path": str(self.path),
            "x": self.view.x,
            "y": self.view.y,
            "zoom": self.view.zoom,
            "theme": self.theme,
            "legend": self.legend,
            "ports": self.ports,
            "sidebar_visible": self.sidebar_visible,
            "crosshair_visible": self.crosshair_visible,
            "current_workspace": self.current_workspace,
            "current_project": self.current_project,
            "follow_active": self.follow_active,
            "live_reload": self.live_reload,
            "saved_at": time.time(),
        }
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(payload, indent=2))
        self.set_message(f"saved state to {self.state_path}", ttl=1.5)

    # ---------- client lifecycle ----------

    def _start_client(self) -> None:
        self.client = WorkspaceClient(self.server_url, actor="tui", timeout=2.0)
        self.stream = EventStream(
            self.client,
            on_event=self._on_stream_event,
            on_state=self._on_stream_state,
            reconnect_initial=SSE_BACKOFF_INITIAL,
            reconnect_max=SSE_BACKOFF_MAX,
        )
        self.stream.start()
        self.conn_state = "client:connecting"
        self.refresh_workspace(silent=True)

    def _stop_client(self) -> None:
        if self.stream:
            self.stream.stop()
            self.stream = None
        self.client = None
        self.conn_state = "local"

    def _start_local_watch(self) -> None:
        self.watcher = FileWatch()
        self.watcher.watch(str(self.path))
        self.watched_diagram_path = str(self.path)

    def _stop_local_watch(self) -> None:
        if self.watcher:
            self.watcher.stop()
            self.watcher = None

    def _on_stream_state(self, state: str) -> None:
        self.conn_state = f"client:{state}" if not state.startswith("client:") else state
        if "error" in state or "disconnected" in state:
            self.conn_error = state

    def _on_stream_event(self, evt: StreamEvent) -> None:
        if evt.type != "event":
            return
        seq = int(evt.raw.get("seq", 0))
        if seq <= self.last_seq:
            return
        self.last_seq = seq
        kind = str(evt.raw.get("kind", "info"))
        summary = str(evt.raw.get("summary", ""))
        actor = str(evt.raw.get("actor", ""))
        feed_kind = "info"
        if "create" in kind:
            feed_kind = "create"
        elif "delete" in kind:
            feed_kind = "delete"
        elif "patch" in kind:
            feed_kind = "patch"
        elif "rename" in kind:
            feed_kind = "update"
        elif "session" in kind:
            feed_kind = "session"
        self.feed.append(FeedItem(
            ts=float(evt.raw.get("ts", time.time())),
            text=f"{summary}" + (f" · {actor}" if actor and actor != "tui" else ""),
            kind=feed_kind,
            source="server",
        ))
        # Follow mode: if the active session points to a diagram we can
        # open, switch to it.
        if kind == "session.active" and self.follow_active:
            data = evt.raw.get("data") or {}
            wid = data.get("workspace") or self.current_workspace
            pid = data.get("project") or self.current_project
            did = data.get("diagram")
            if wid:
                self.current_workspace = wid
            if pid:
                self.current_project = pid
            if did and self.client:
                self.open_server_diagram(wid, pid, did, silent=True)
        # Live reload: if the active diagram was just put/patched, refresh it.
        if self.live_reload and kind in ("diagram.put", "diagram.patch"):
            wid = evt.raw.get("workspace")
            pid = evt.raw.get("project")
            did = evt.raw.get("diagram")
            if (wid == self.current_workspace and pid == self.current_project
                    and did and self.doc.get("id") == did):
                self.open_server_diagram(wid, pid, did, silent=True)

    def open_server_diagram(self, workspace: str, project: str, diagram: str, *, silent: bool = False) -> None:
        if not self.client:
            return
        try:
            resp = self.client.get_diagram(id=diagram, workspace=workspace, project=project)
        except WorkspaceError as e:
            if not silent:
                self.set_message(str(e), ttl=3.0)
            return
        self.doc = resp.get("document") or {}
        self.path = Path(resp.get("path", self.path))
        # Re-fit the viewport to the new diagram.
        self.view = self._center_target()
        if not silent:
            self.set_message(f"opened {workspace}/{project}/{diagram}", ttl=1.5)
        # Re-watch the on-disk file too in case someone edits it directly.
        if self.watcher:
            self.watcher.stop()
        if str(self.path) != self.watched_diagram_path:
            self._start_local_watch()

    # ---------- message ----------

    def set_message(self, text: str, *, ttl: float = 1.2) -> None:
        self.message = text
        self.message_until = time.monotonic() + ttl

    def status_text(self) -> str:
        now = time.monotonic()
        if self.message and now < self.message_until:
            return self.message
        self.message = ""
        return ""

    def _local_log(self, text: str, kind: str = "local") -> None:
        self.feed.append(FeedItem(ts=time.time(), text=text, kind=kind, source="local"))

    # ---------- main loop ----------

    def run(self) -> None:
        try:
            with RawMode():
                sys.stdout.write("\033[?1049h\033[?25l")
                try:
                    while True:
                        self.draw()
                        self._poll_background()
                        key = self.read_key()
                        if not self.handle_key(key):
                            break
                finally:
                    sys.stdout.write("\033[?25h\033[?1049l")
                    sys.stdout.flush()
        finally:
            self._stop_client()
            self._stop_local_watch()

    def _poll_background(self) -> None:
        # Drain SSE events + file-watch changes.
        if self.stream:
            for evt in self.stream.events(block=False, timeout=0.0):
                # _on_stream_event already pushed to feed; nothing to do.
                pass
        if self.watcher and self.watched_diagram_path:
            for ch in self.watcher.changes(timeout=0.0):
                if self.live_reload and ch.path == self.watched_diagram_path:
                    try:
                        self.doc = load_doc(Path(ch.path), safe=True)
                        self._local_log(f"reloaded {Path(ch.path).name}", "local")
                    except Exception as e:
                        self._local_log(f"reload error: {e}", "error")

    # ---------- drawing ----------

    def frame(self, width: int, height: int) -> str:
        self._tick_pet()
        width = max(60, width)
        height = max(16, height)
        if self.overlay == "chat":
            return self._chat_frame(width, height)
        if self.overlay == "help":
            return self._help_frame(width, height)
        if self.overlay == "command":
            return self._command_frame(width, height)
        if self.overlay == "search":
            return self._search_frame(width, height)
        if self.overlay == "prompt":
            return self._prompt_frame(width, height)
        if self.mode == "dashboard":
            return self._dashboard_frame(width, height)
        return self._canvas_frame(width, height)

    def _dashboard_frame(self, width: int, height: int) -> str:
        body_h, left_w, right_w = self._layout_dims(width, height, allow_hide=False)
        header = self._header_lines(width, "dashboard", self._sub_title())

        # Left: workspaces + projects + diagrams in active project.
        ws_lines: list[str] = []
        ws_lines.append(style("workspaces", self.color, BOLD, MAGENTA))
        if not self.workspaces and self.client_mode:
            ws_lines.append(style("  (not connected)", self.color, DIM))
        for w in self.workspaces:
            marker = "● " if w.get("id") == self.current_workspace else "  "
            label = w.get("title") or w.get("id", "?")
            line = f"{marker}{label}"
            if w.get("id") == self.current_workspace:
                line = style(line, self.color, BOLD, CYAN)
            ws_lines.append(line)
        ws_lines.append("")
        ws_lines.append(style("projects", self.color, BOLD, MAGENTA))
        for p in self.projects:
            marker = "● " if p.get("id") == self.current_project else "  "
            label = p.get("title") or p.get("id", "?")
            line = f"{marker}{label}"
            if p.get("id") == self.current_project:
                line = style(line, self.color, BOLD, CYAN)
            ws_lines.append(line)
        ws_lines.append("")
        ws_lines.append(style("diagrams", self.color, BOLD, MAGENTA))
        for d in self.project_diagrams:
            marker = "● " if d.get("id") == (self.doc.get("id") if self.doc else None) else "  "
            label = d.get("title") or d.get("id", "?")
            ws_lines.append(f"{marker}{label}")
        if not self.project_diagrams:
            ws_lines.append(style("  (no diagrams)", self.color, DIM))
        ws_lines.append("")
        ws_lines.append(style("actions", self.color, BOLD, MAGENTA))
        ws_lines.append("W new ws  ·  P new proj")
        ws_lines.append("D delete   ·  u refresh")
        ws_lines.append("C copy here")
        ws_lines.append("f follow   ·  L live")

        # Right: live activity feed + preview.
        feed_body = [style("activity", self.color, BOLD, MAGENTA), ""]
        now = time.time()
        for item in list(self.feed)[-max(2, body_h - 16):]:
            tag = {
                "create": "+", "update": "↻", "delete": "−",
                "patch": "~", "session": "@", "error": "!",
            }.get(item.kind, "·")
            color_map = {
                "create": GREEN, "delete": RED, "patch": YELLOW,
                "session": CYAN, "error": RED, "local": DIM,
            }
            pen = color_map.get(item.kind, DIM)
            head = f" {tag} {short_ago(item.ts, now)} "
            head = style(head, self.color, pen, BOLD)
            text = plain_fit(item.text, max(8, right_w - len(strip_ansi(head)) - 1))
            feed_body.append(head + text)
        if not self.feed:
            feed_body.append(style("  (empty)", self.color, DIM))

        preview_doc = self.doc
        if self.project_diagrams:
            target = next((d for d in self.project_diagrams if d.get("id") == self.doc.get("id")), None)
            if target:
                preview_doc = {"id": target.get("id", "preview"), "title": target.get("title", target.get("id", "preview")),
                                "kind": target.get("kind", "unknown"),
                                "nodes": [], "edges": []}
        preview = Renderer(
            preview_doc,
            max(10, right_w),
            max(5, body_h - len(feed_body) - 6),
            color=self.color,
            theme=self.theme,
            view_x=0,
            view_y=0,
            zoom=1,
            legend=False,
            ports=False,
        ).render().splitlines()
        feed_body.append("")
        feed_body.append(style("preview", self.color, BOLD, MAGENTA))
        feed_body.extend(preview)

        left = panel("hubs", left_w, body_h, ws_lines, color=self.color, active=True)
        right = panel("activity", right_w, body_h, feed_body, color=self.color, active=False)
        rows = header
        for a, b in zip(left, right):
            rows.append(a + " " + b)
        rows.append(style(plain_fit(f" {self._pressure_line(self.doc, 30)}  {self._status_text()}", width), self.color, DIM))
        rows.append(style(plain_fit(
            f" {self._example_dial(24)}  enter open · / search · : command · ? help · q quit",
            width,
        ), self.color, DIM))
        rows = self._overlay_pet(rows[:height], width)
        return "\n".join(rows[:height]) + "\n"

    def _canvas_frame(self, width: int, height: int) -> str:
        body_h, side_w, canvas_w = self._layout_dims(width, height, allow_hide=True)
        title = self.doc.get("title") or self.doc.get("id") or self.path.name
        header = self._header_lines(width, title, self._path_label())

        sidebar = self._sidebar_lines(side_w, body_h) if self.sidebar_visible else []
        rendered = Renderer(
            self.doc,
            max(5, canvas_w),
            max(5, body_h),
            ascii_mode=False,
            color=self.color,
            theme=self.theme,
            view_x=self.view.x,
            view_y=self.view.y,
            zoom=self.view.zoom,
            legend=self.legend,
            ports=self.ports,
        ).render().splitlines()
        if self.crosshair_visible:
            rendered = self._with_crosshair(rendered, max(5, canvas_w), max(5, body_h))
        canvas_panel = panel("canvas", canvas_w, body_h, rendered, color=self.color, active=True)
        rows = header
        if self.sidebar_visible:
            left = panel("hub", side_w, body_h, sidebar, color=self.color, active=False)
            for a, b in zip(left, canvas_panel):
                rows.append(a + " " + b)
        else:
            rows.extend(canvas_panel)
        rows.append(style(plain_fit(f" {self._pressure_line(self.doc, 30)}  {self._status_text()}", width), self.color, DIM))
        rows.append(style(plain_fit(
            " a ask · m chat · space center · x crosshair · b rail · f follow · L live · "
            "/ search · : command · ? help · esc dashboard · q quit",
            width,
        ), self.color, DIM))
        rows = self._overlay_pet(rows[:height], width)
        return "\n".join(rows[:height]) + "\n"

    def _help_frame(self, width: int, height: int) -> str:
        body_h = max(8, height - 4)
        header = self._header_lines(width, "help", "keymap")
        body = [
            style("navigate", self.color, BOLD, MAGENTA),
            "  j/k or arrows   select (dashboard) / pan (canvas)",
            "  enter / space   open example / center (canvas)",
            "  n / p           next / previous example",
            "  0               reset viewport",
            "  +/-  or  =/_    zoom (relative to canvas center)",
            "  f               fit / toggle follow-active",
            "",
            style("workspace", self.color, BOLD, MAGENTA),
            "  W               new workspace (prompts for name)",
            "  P               new project (prompts for name)",
            "  D               delete current workspace/project/diagram",
            "  R               rename current",
            "  C               copy current diagram into active project",
            "  u               refresh from server",
            "  L               toggle live reload of current diagram",
            "",
            style("view", self.color, BOLD, MAGENTA),
            "  t               cycle theme",
            "  c               toggle color",
            "  g               toggle legend",
            "  o               toggle port markers",
            "  x               toggle crosshair",
            "  b               toggle info rail",
            "  s               save session state",
            "  r               reload current file",
            "",
            style("overlays", self.color, BOLD, MAGENTA),
            "  :               command palette",
            "  /               search diagrams",
            "  ?               this help",
            "  m or esc        chat overlay (harness pet)",
            "  q               quit",
        ]
        rows = header
        rows.extend(panel("help", width, body_h, body, color=self.color, active=True))
        rows.append(style(plain_fit(" press any key to dismiss ", width), self.color, DIM))
        return "\n".join(rows[:height]) + "\n"

    def _command_frame(self, width: int, height: int) -> str:
        body_h = max(8, height - 6)
        header = self._header_lines(width, "command palette", "type to filter")
        commands = self._commands()
        q = self.prompt_buffer.lower()
        matches = [(k, label) for k, label in commands if q in k.lower() or q in label.lower()]
        body: list[str] = []
        for i, (k, label) in enumerate(matches[: body_h - 2]):
            mark = "● " if i == 0 else "  "
            body.append(style(mark, self.color, GREEN) + style(k, self.color, BOLD, CYAN) + "  " + label)
        if not matches:
            body.append(style("  no matches", self.color, DIM))
        rows = header
        rows.extend(panel("commands", width, body_h, body, color=self.color, active=True))
        rows.append(rule("input", width, color=self.color, active=True))
        rows.append(fit_ansi(style(":" + self.prompt_buffer, self.color, BOLD, CYAN), width))
        rows.append(style(plain_fit(" enter run · esc cancel · up/down select ", width), self.color, DIM))
        return "\n".join(rows[:height]) + "\n"

    def _search_frame(self, width: int, height: int) -> str:
        body_h = max(8, height - 6)
        header = self._header_lines(width, "search", "type to filter, enter to open")
        matches = self._search(self.prompt_buffer)
        body: list[str] = []
        for i, m in enumerate(matches[: body_h - 2]):
            mark = "● " if i == self.search_index else "  "
            where = m.get("where", "")
            text = f"{m.get('title', m.get('id', '?'))}"
            body.append(style(mark, self.color, GREEN) + text + style(f"  {where}", self.color, DIM))
        if not matches:
            body.append(style("  no matches", self.color, DIM))
        rows = header
        rows.extend(panel("results", width, body_h, body, color=self.color, active=True))
        rows.append(rule("input", width, color=self.color, active=True))
        rows.append(fit_ansi(style("/" + self.prompt_buffer, self.color, BOLD, CYAN), width))
        rows.append(style(plain_fit(" enter open · esc cancel · up/down select ", width), self.color, DIM))
        return "\n".join(rows[:height]) + "\n"

    def _prompt_frame(self, width: int, height: int) -> str:
        body_h = max(8, height - 6)
        header = self._header_lines(width, "prompt", self.prompt_label)
        body: list[str] = []
        if self.prompt_choices:
            for i, (k, label) in enumerate(self.prompt_choices):
                mark = "● " if i == 0 else "  "
                body.append(style(mark, self.color, GREEN) + style(k, self.color, BOLD, CYAN) + "  " + label)
        else:
            body.append(style("  enter to confirm · esc to cancel", self.color, DIM))
        rows = header
        rows.extend(panel("input", width, body_h, body, color=self.color, active=True))
        rows.append(rule("typed", width, color=self.color, active=True))
        rows.append(fit_ansi(style(self.prompt_buffer, self.color, BOLD, CYAN), width))
        rows.append(style(plain_fit(" enter confirm · esc cancel ", width), self.color, DIM))
        return "\n".join(rows[:height]) + "\n"

    def _chat_frame(self, width: int, height: int) -> str:
        body_h = max(8, height - 6)
        header = self._header_lines(width, "harness chat", f"{self.provider}/{self.model}")
        transcript = self._chat_transcript(width - 2, body_h)
        input_line = "> " + self.chat_input
        if self.pet_state == "thinking":
            input_line += "  " + self._spinner()
        rows = header
        rows.extend(panel("transcript", width, body_h, transcript, color=self.color, active=True))
        rows.append(rule("prompt", width, color=self.color, active=True))
        rows.append(fit_ansi(style(input_line[-max(1, width - 1):], self.color, BOLD, CYAN), width))
        rows.append(style(plain_fit(f" {self._pressure_line(self.doc, 30)}  {self._status_text()}", width), self.color, DIM))
        rows.append(style(plain_fit(" enter send · a quick ask · m/esc close · q quit", width), self.color, DIM))
        return "\n".join(rows[:height]) + "\n"

    # ---------- layout / chrome ----------

    def _layout_dims(self, width: int, height: int, *, allow_hide: bool = False) -> tuple[int, int, int]:
        header_h = 2
        footer_h = 2
        body_h = max(8, height - header_h - footer_h)
        if not allow_hide:
            side_w = min(42, max(30, width // 3))
        else:
            side_w = min(24, max(20, width // 5)) if self.sidebar_visible else 0
        main_w = max(20, width - side_w - (1 if side_w else 0))
        return body_h, side_w, main_w

    def _header_lines(self, width: int, title: str, subtitle: str) -> list[str]:
        left = f" {self._spinner()} diagram-junky · {title}"
        row1 = style(plain_fit(left, width), self.color, BOLD, self._pulse_code())
        conn = self._conn_indicator()
        status = (
            f" {subtitle} · {conn} · theme={self.theme} · zoom={self.view.zoom:.2f} "
            f"{self._zoom_dial(18)} · view=({self.view.x:.1f},{self.view.y:.1f})"
        )
        row2 = style(plain_fit(status, width), self.color, DIM)
        return [row1, row2]

    def _conn_indicator(self) -> str:
        if not self.client_mode:
            return "local"
        if "error" in self.conn_state or "disconnected" in self.conn_state:
            return style("client:offline", self.color, RED, BOLD)
        if "connecting" in self.conn_state:
            return style("client:connecting", self.color, YELLOW)
        if "connected" in self.conn_state:
            return style("client:live", self.color, GREEN, BOLD)
        return style(self.conn_state, self.color, DIM)

    def _sub_title(self) -> str:
        if self.client_mode:
            return f"{self.current_workspace}/{self.current_project}"
        return "local mode"

    def _path_label(self) -> str:
        if self.client_mode:
            return f"{self.current_workspace}/{self.current_project}/{self.doc.get('id', '?')}"
        rel = self.path.relative_to(ROOT) if self.path.is_relative_to(ROOT) else self.path
        return str(rel)

    def _zoom_dial(self, width: int) -> str:
        width = max(8, width)
        min_z, max_z = 0.1, 4.0
        t = (max(min_z, min(max_z, self.view.zoom)) - min_z) / (max_z - min_z)
        pos = round(t * (width - 1))
        chars = ["─"] * width
        for dx, ch in [(-1, "·"), (0, "●"), (1, "·")]:
            j = pos + dx
            if 0 <= j < width:
                chars[j] = ch
        return "".join(chars)

    def _example_dial(self, width: int) -> str:
        width = max(8, width)
        total = max(1, len(self.examples))
        pos = 0 if total == 1 else round((self.example_index / (total - 1)) * (width - 1))
        chars = ["─"] * width
        chars[pos] = "●"
        return "".join(chars) + f" {self.example_index + 1}/{total}"

    def _sidebar_lines(self, width: int, height: int) -> list[str]:
        lines: list[str] = []
        lines.append(style("workspace", self.color, BOLD, MAGENTA))
        lines.append(self.current_workspace)
        lines.append(f"project {self.current_project}")
        lines.append(f"diagrams {len(self.project_diagrams)}")
        lines.append("W new ws · P project")
        lines.append("C copy · u refresh")
        lines.append("")
        lines.append(style("info", self.color, BOLD, MAGENTA))
        lines.append(self.doc.get("id") or self.path.stem)
        lines.append(f"nodes  {len(self.doc.get('nodes', []))}")
        lines.append(f"edges  {len(self.doc.get('edges', []))}")
        lines.append("")
        lines.append(style("view", self.color, BOLD, MAGENTA))
        lines.append(f"zoom   {self.view.zoom:.2f}")
        lines.append(self._zoom_dial(max(8, width)))
        lines.append("")
        lines.append(style("flags", self.color, BOLD, MAGENTA))
        lines.append(f"follow  {'on' if self.follow_active else 'off'}")
        lines.append(f"live    {'on' if self.live_reload else 'off'}")
        lines.append(f"ports   {'on' if self.ports else 'off'}")
        lines.append(f"legend  {'on' if self.legend else 'off'}")
        lines.append(f"cross   {'on' if self.crosshair_visible else 'off'}")
        lines.append("")
        lines.append(style("b hides rail", self.color, DIM))
        return [fit_ansi(line, width) for line in lines[:height]]

    def _chat_transcript(self, width: int, height: int) -> list[str]:
        lines: list[str] = []
        lines.append(style("pet", self.color, BOLD, MAGENTA))
        pet = self._pet_lines()
        lines.extend(pet[:5])
        lines.append("")
        lines.append(style(f"model {self.provider}/{self.model}", self.color, DIM))
        lines.append(style(
            f"diagram {self.doc.get('id', self.path.stem)} · {self.current_workspace}/{self.current_project}",
            self.color, DIM,
        ))
        lines.append("")
        if not self.chat_history:
            lines.append(style("no messages yet", self.color, DIM))
            lines.append("type a prompt below, or press a for a quick read")
        else:
            for role, text in self.chat_history[-8:]:
                label = style("you", self.color, BOLD, CYAN) if role == "you" else style("pet", self.color, BOLD, MAGENTA)
                lines.append(label)
                lines.extend("  " + ln for ln in self._wrap_plain(text, max(8, width - 2), 8))
                lines.append("")
        return [fit_ansi(line, width) for line in lines[-height:]]

    def _wrap_plain(self, text: str, width: int, max_lines: int) -> list[str]:
        if width <= 0 or max_lines <= 0:
            return []
        words = strip_ansi(text).replace("\n", " \n ").split()
        lines: list[str] = []
        cur = ""
        for word in words:
            if word == "\n":
                if cur:
                    lines.append(cur)
                    cur = ""
                continue
            if not cur:
                cur = word
            elif len(cur) + 1 + len(word) <= width:
                cur += " " + word
            else:
                lines.append(cur)
                cur = word
            if len(lines) >= max_lines:
                break
        if cur and len(lines) < max_lines:
            lines.append(cur)
        return lines[:max_lines]

    # ---------- crosshair / pet ----------

    def _replace_visible_char(self, line: str, col: int, ch: str) -> str:
        if col < 0:
            return line
        out = ""
        visible = 0
        i = 0
        replaced = False
        while i < len(line):
            if line[i] == "\033":
                m = ANSI_RE.match(line, i)
                if m:
                    out += m.group(0)
                    i = m.end()
                    continue
            if visible == col:
                out += ch
                replaced = True
                i += 1
                visible += 1
                continue
            out += line[i]
            i += 1
            visible += 1
        if not replaced:
            out += " " * max(0, col - visible) + ch
        if "\033[" in out and not out.endswith(RESET):
            out += RESET
        return out

    def _with_crosshair(self, lines: list[str], width: int, height: int) -> list[str]:
        out = list(lines[:height])
        while len(out) < height:
            out.append("")
        cx = max(0, width // 2)
        cy = max(0, height // 2)
        mark = style("┼", self.color, BOLD, MAGENTA)
        arm = style("·", self.color, DIM)
        for dx in (-2, -1, 1, 2):
            out[cy] = self._replace_visible_char(out[cy], cx + dx, arm)
        for dy in (-2, -1, 1, 2):
            y = cy + dy
            if 0 <= y < len(out):
                out[y] = self._replace_visible_char(out[y], cx, arm)
        out[cy] = self._replace_visible_char(out[cy], cx, mark)
        return out

    def _pet_lines(self) -> list[str]:
        if self.pet_state == "thinking":
            eyes = ["o.o", "O.o", "o.O", "O.O"][self._tick(7.0) % 4]
            return [" /\\ ", f"({eyes})", " /|\\", " / \\", " ask "]
        if self.pet_state == "error":
            return [" /\\ ", "(x.x)", " /|\\", " / \\", " err "]
        if self.pet_state == "done":
            return [" /\\ ", "(^.^)", " /|\\", " / \\", " ok  "]
        return [" /\\ ", "(·.·)", " /|\\", " / \\", " ai  "]

    def _tick_pet(self) -> None:
        if self.pet_state in ("done", "error") and self.pet_done_at > 0:
            if time.monotonic() - self.pet_done_at > 6.0:
                self.pet_state = "idle"
                self.pet_done_at = 0.0

    def _overlay_pet(self, rows: list[str], width: int) -> list[str]:
        if width < 48 or len(rows) < 8:
            return rows
        pet = self._pet_lines()
        start = max(2, len(rows) - 7)
        col = max(0, width - 7)
        out = list(rows)
        for i, line in enumerate(pet):
            y = start + i
            if y >= len(out):
                break
            out[y] = fit_ansi(out[y], col) + style(line, self.color, DIM)
        return out

    # ---------- chat / model ----------

    def submit_chat(self, text: str | None = None) -> None:
        prompt = (text if text is not None else self.chat_input).strip()
        if not prompt:
            prompt = "Give me one useful read of this diagram and one next action."
        self.chat_input = ""
        self.chat_history.append(("you", prompt))
        self.ask_model(self.doc, user_text=prompt)

    def _clean_model_text(self, raw: str) -> str:
        text = strip_ansi(raw).strip()
        m = re.search(r"<response[^>]*>(.*?)</response>", text, re.DOTALL)
        if m:
            return m.group(1).strip()
        return text

    def _model_prompt(self, doc: dict[str, Any], user_text: str = "") -> str:
        nodes = [n.get("id", "?") for n in doc.get("nodes", [])]
        edges = [f"{e.get('source', {}).get('node', '?')}->{e.get('target', {}).get('node', '?')}" for e in doc.get("edges", [])]
        return (
            "You are the diagram-junky embedded harness pet. Be concise and practical. "
            "Answer the user's diagram request using the summary. If suggesting changes, propose them as workspace/project-safe operations, not raw filesystem mutation.\n"
            f"user_request: {user_text or 'Give one useful insight plus one next action.'}\n"
            f"id: {doc.get('id')}\n"
            f"title: {doc.get('title')}\n"
            f"kind: {doc.get('kind')}\n"
            f"nodes: {', '.join(nodes)}\n"
            f"edges: {', '.join(edges)}\n"
            f"pressure: {self._pressure_score(doc)[0]}\n"
            f"workspace: {self.current_workspace}\n"
            f"project: {self.current_project}\n"
            "You may propose workspace/project operations, but do not assume they are applied until confirmed.\n"
        )

    def ask_model(self, doc: dict[str, Any], user_text: str = "") -> None:
        if self.model_thread and self.model_thread.is_alive():
            self.set_message("model already thinking", ttl=1.0)
            return
        self.pet_state = "thinking"
        self.pet_done_at = 0.0
        self.set_message(f"asking {self.provider}/{self.model}", ttl=1.2)

        def run() -> None:
            try:
                exe = PROJECT_ROOT / "cortex-mk3"
                if not exe.exists():
                    raise FileNotFoundError(str(exe))
                manifest = ROOT / "manifests" / "agents" / "diagram-junky" / "agent.yml"
                cmd = [
                    str(exe),
                    "--provider", self.provider,
                    "--model", self.model,
                    "--raw",
                    "--manifest", str(manifest),
                    "run", "-p", self._model_prompt(doc, user_text),
                ]
                proc = subprocess.run(cmd, cwd=PROJECT_ROOT, text=True, capture_output=True, timeout=self.model_timeout)
                if proc.returncode != 0:
                    raise RuntimeError((proc.stderr or proc.stdout).strip()[:240])
                self.model_output = self._clean_model_text(proc.stdout)[-800:]
                self.chat_history.append(("pet", self.model_output or "model done"))
                self.pet_state = "done"
                self.pet_done_at = time.monotonic()
                self.set_message("model replied", ttl=2.0)
            except Exception as e:
                self.pet_state = "error"
                self.pet_done_at = time.monotonic()
                self.chat_history.append(("pet", f"error: {e}"))
                self.set_message(f"model error: {e}", ttl=4.0)

        self.model_thread = threading.Thread(target=run, daemon=True)
        self.model_thread.start()

    # ---------- workspace CRUD ----------

    def refresh_workspace(self, *, silent: bool = False) -> None:
        if not self.client:
            self.set_message("not in client mode", ttl=1.5)
            return
        try:
            self.workspaces = self.client.list_workspaces()
            self.projects = self.client.list_projects(workspace=self.current_workspace)
            self.project_diagrams = self.client.list_diagrams(
                workspace=self.current_workspace, project=self.current_project,
            )
            if not silent:
                self.set_message("workspace refreshed", ttl=1.0)
        except WorkspaceError as e:
            self.set_message(str(e), ttl=3.0)
            self._local_log(f"refresh failed: {e}", "error")

    def _create_workspace_prompt(self) -> None:
        if not self.client:
            self.set_message("client mode only", ttl=2.0)
            return
        self._open_prompt(
            "new workspace name",
            default=f"ws-{int(time.time())}",
            on_submit=self._create_workspace_submit,
        )

    def _create_workspace_submit(self, name: str) -> None:
        name = name.strip()
        if not name:
            return
        try:
            self.client.create_workspace(workspace=name, title=name)
            self.current_workspace = name
            self.current_project = "inbox"
            self._local_log(f"workspace created: {name}", "create")
            self.set_message(f"workspace created: {name}", ttl=2.0)
            self.refresh_workspace(silent=True)
        except WorkspaceError as e:
            self.set_message(str(e), ttl=3.0)
            self._local_log(f"workspace create failed: {e}", "error")

    def _create_project_prompt(self) -> None:
        if not self.client:
            self.set_message("client mode only", ttl=2.0)
            return
        self._open_prompt(
            f"new project in {self.current_workspace}",
            default=f"project-{int(time.time())}",
            on_submit=self._create_project_submit,
        )

    def _create_project_submit(self, name: str) -> None:
        name = name.strip()
        if not name:
            return
        try:
            self.client.create_project(workspace=self.current_workspace, project=name, title=name)
            self.current_project = name
            self._local_log(f"project created: {self.current_workspace}/{name}", "create")
            self.set_message(f"project created: {name}", ttl=2.0)
            self.refresh_workspace(silent=True)
        except WorkspaceError as e:
            self.set_message(str(e), ttl=3.0)
            self._local_log(f"project create failed: {e}", "error")

    def _copy_current_to_project(self) -> None:
        if not self.client:
            self.set_message("client mode only", ttl=2.0)
            return
        try:
            self.client.put_diagram(
                id=self.doc.get("id") or self.path.stem,
                document=self.doc,
                workspace=self.current_workspace,
                project=self.current_project,
            )
            self._local_log(
                f"copied {self.doc.get('id')} → {self.current_workspace}/{self.current_project}",
                "create",
            )
            self.set_message(f"copied {self.doc.get('id')} → {self.current_workspace}/{self.current_project}", ttl=2.0)
            self.refresh_workspace(silent=True)
        except WorkspaceError as e:
            self.set_message(str(e), ttl=3.0)
            self._local_log(f"copy failed: {e}", "error")

    def _delete_current(self) -> None:
        if not self.client:
            self.set_message("client mode only", ttl=2.0)
            return
        # Chooser: workspace, project, or diagram?
        choices = [
            ("workspace", f"workspace '{self.current_workspace}'"),
            ("project",   f"project '{self.current_workspace}/{self.current_project}'"),
            ("diagram",   f"diagram '{self.doc.get('id', '?')}'"),
        ]
        self._open_chooser("delete which?", choices, on_submit=self._delete_submit)

    def _delete_submit(self, kind: str) -> None:
        try:
            if kind == "workspace":
                self.client.delete_workspace(workspace=self.current_workspace)
                self._local_log(f"workspace deleted: {self.current_workspace}", "delete")
                self.current_workspace = "default"
                self.current_project = "inbox"
            elif kind == "project":
                self.client.delete_project(workspace=self.current_workspace, project=self.current_project)
                self._local_log(f"project deleted: {self.current_workspace}/{self.current_project}", "delete")
                self.current_project = "inbox"
            elif kind == "diagram":
                self.client.delete_diagram(
                    id=self.doc.get("id"),
                    workspace=self.current_workspace,
                    project=self.current_project,
                )
                self._local_log(f"diagram deleted: {self.doc.get('id')}", "delete")
            self.set_message(f"deleted: {kind}", ttl=2.0)
            self.refresh_workspace(silent=True)
        except WorkspaceError as e:
            self.set_message(str(e), ttl=3.0)
            self._local_log(f"delete failed: {e}", "error")

    def _rename_current(self) -> None:
        if not self.client:
            self.set_message("client mode only", ttl=2.0)
            return
        choices = [
            ("workspace", f"workspace '{self.current_workspace}'"),
            ("project",   f"project '{self.current_workspace}/{self.current_project}'"),
        ]
        self._open_chooser("rename which?", choices, on_submit=self._rename_kind)

    def _rename_kind(self, kind: str) -> None:
        if kind == "workspace":
            self._open_prompt("new workspace title", default=self.current_workspace, on_submit=self._rename_workspace_submit)
        else:
            self._open_prompt("new project title", default=self.current_project, on_submit=self._rename_project_submit)

    def _rename_workspace_submit(self, title: str) -> None:
        try:
            self.client.rename_workspace(workspace=self.current_workspace, title=title)
            self._local_log(f"workspace renamed: {self.current_workspace} → {title}", "update")
            self.refresh_workspace(silent=True)
        except WorkspaceError as e:
            self.set_message(str(e), ttl=3.0)

    def _rename_project_submit(self, title: str) -> None:
        try:
            self.client.rename_project(workspace=self.current_workspace, project=self.current_project, title=title)
            self._local_log(f"project renamed: {self.current_project} → {title}", "update")
            self.refresh_workspace(silent=True)
        except WorkspaceError as e:
            self.set_message(str(e), ttl=3.0)

    # ---------- prompt + chooser overlays ----------

    def _open_prompt(self, label: str, default: str, on_submit: Any) -> None:
        self.overlay = "prompt"
        self.prompt_label = label
        self.prompt_buffer = default
        self.prompt_choices = []
        self._prompt_on_submit = on_submit

    def _open_chooser(self, label: str, choices: list[tuple[str, str]], on_submit: Any) -> None:
        self.overlay = "prompt"
        self.prompt_label = label
        self.prompt_buffer = ""
        self.prompt_choices = choices
        self._prompt_on_submit = on_submit

    # ---------- command palette ----------

    def _commands(self) -> list[tuple[str, str]]:
        cmds: list[tuple[str, str]] = [
            ("dashboard", "go to dashboard"),
            ("canvas", "go to canvas mode"),
            ("new workspace", "create a new workspace"),
            ("new project", "create a new project in active workspace"),
            ("delete current", "delete workspace/project/diagram"),
            ("rename current", "rename workspace/project"),
            ("refresh", "refresh workspace state from server"),
            ("copy to project", "copy current diagram into active project"),
            ("follow on", "auto-open the active session's diagram"),
            ("follow off", "stop following the active session"),
            ("live on", "auto-reload the current diagram on server change"),
            ("live off", "stop auto-reloading the current diagram"),
            ("search", "search diagrams (/)"),
            ("help", "show keymap overlay (?)"),
            ("toggle theme", "cycle theme (default / mono / neon)"),
            ("toggle color", "enable/disable ANSI colors"),
            ("toggle legend", "show renderer legend"),
            ("toggle ports", "show node port markers"),
            ("toggle crosshair", "show canvas center crosshair"),
            ("toggle sidebar", "show info rail"),
            ("fit", "fit viewport to diagram bounds"),
            ("center", "smooth center animation"),
            ("zoom in", "zoom in (centered)"),
            ("zoom out", "zoom out (centered)"),
            ("zoom reset", "reset zoom to 1x"),
            ("save state", "persist viewport + session state"),
            ("reload", "reload current file from disk"),
            ("chat", "open harness pet chat (m)"),
            ("ask model", "quick ask the harness pet (a)"),
            ("quit", "exit the TUI"),
        ]
        return cmds

    def _run_command(self, name: str) -> bool:
        """Run a command by name. Returns False if the TUI should exit."""
        name = name.lower()
        if name in ("dashboard",):
            self.mode = "dashboard"
            self.overlay = None
        elif name in ("canvas",):
            self.mode = "canvas"
            self.overlay = None
        elif name in ("new workspace",):
            self.overlay = None
            self._create_workspace_prompt()
        elif name in ("new project",):
            self.overlay = None
            self._create_project_prompt()
        elif name in ("delete current",):
            self.overlay = None
            self._delete_current()
        elif name in ("rename current",):
            self.overlay = None
            self._rename_current()
        elif name in ("refresh",):
            self.refresh_workspace()
        elif name in ("copy to project",):
            self._copy_current_to_project()
        elif name in ("follow on",):
            self.follow_active = True
            self.set_message("follow: on", ttl=1.0)
        elif name in ("follow off",):
            self.follow_active = False
            self.set_message("follow: off", ttl=1.0)
        elif name in ("live on",):
            self.live_reload = True
            self.set_message("live reload: on", ttl=1.0)
        elif name in ("live off",):
            self.live_reload = False
            self.set_message("live reload: off", ttl=1.0)
        elif name in ("search",):
            self.overlay = "search"
            self.prompt_buffer = ""
            self.search_index = 0
        elif name in ("help",):
            self.overlay = "help"
        elif name in ("toggle theme",):
            self.cycle_theme()
        elif name in ("toggle color",):
            self.color = not self.color
        elif name in ("toggle legend",):
            self.legend = not self.legend
        elif name in ("toggle ports",):
            self.ports = not self.ports
        elif name in ("toggle crosshair",):
            self.crosshair_visible = not self.crosshair_visible
        elif name in ("toggle sidebar",):
            self.sidebar_visible = not self.sidebar_visible
        elif name in ("fit",):
            self.fit()
        elif name in ("center",):
            self.animate_center()
        elif name in ("zoom in",):
            self.zoom(1.15)
        elif name in ("zoom out",):
            self.zoom(1 / 1.15)
        elif name in ("zoom reset",):
            self.view = View()
            self.set_message("zoom reset", ttl=1.0)
        elif name in ("save state",):
            self.save_state()
        elif name in ("reload",):
            self.doc = load_doc(self.path, safe=True)
            self.set_message(f"reloaded {self.path.name}", ttl=1.2)
        elif name in ("chat",):
            self.overlay = "chat"
        elif name in ("ask model",):
            self.ask_model(self.doc, "Give me one useful read of this diagram and one next action.")
        elif name in ("quit",):
            return False
        else:
            self.set_message(f"unknown command: {name}", ttl=2.0)
        return True

    # ---------- search ----------

    def _search(self, query: str) -> list[dict[str, Any]]:
        """Return candidate diagrams matching the query, ranked by recency.

        Searches across local examples + the active project's diagrams in
        client mode. Always returns at least the active document.
        """
        candidates: list[dict[str, Any]] = []
        for p in self.examples:
            doc = load_doc(p, safe=True)
            candidates.append({
                "id": doc.get("id") or p.stem,
                "title": doc.get("title") or p.stem,
                "kind": doc.get("kind", "unknown"),
                "where": "example",
                "path": p,
            })
        if self.client_mode:
            for d in self.project_diagrams:
                candidates.append({
                    "id": d.get("id"),
                    "title": d.get("title") or d.get("id"),
                    "kind": d.get("kind", "unknown"),
                    "where": f"{self.current_workspace}/{self.current_project}",
                    "_remote": True,
                    "remote": (self.current_workspace, self.current_project, d.get("id")),
                })
            for w in self.workspaces:
                if w.get("id") == self.current_workspace:
                    continue
                try:
                    for p in self.client.list_projects(workspace=w["id"]):
                        for d in self.client.list_diagrams(workspace=w["id"], project=p["id"]):
                            candidates.append({
                                "id": d.get("id"),
                                "title": d.get("title") or d.get("id"),
                                "kind": d.get("kind", "unknown"),
                                "where": f"{w['id']}/{p['id']}",
                                "_remote": True,
                                "remote": (w["id"], p["id"], d.get("id")),
                            })
                except WorkspaceError:
                    pass

        if not query:
            return candidates[:30]
        q = query.lower()
        scored: list[tuple[int, dict[str, Any]]] = []
        for c in candidates:
            title = (c.get("title") or "").lower()
            cid = (c.get("id") or "").lower()
            score = 0
            if q in title:
                score += 100
            if q in cid:
                score += 60
            if q in (c.get("where") or "").lower():
                score += 20
            if score:
                scored.append((score, c))
        scored.sort(key=lambda x: -x[0])
        return [c for _, c in scored[:30]]

    def _open_search_result(self, result: dict[str, Any]) -> None:
        if result.get("_remote") and self.client:
            wid, pid, did = result["remote"]
            self.current_workspace = wid
            self.current_project = pid
            self.open_server_diagram(wid, pid, did)
            self.refresh_workspace(silent=True)
        else:
            p = result.get("path")
            if p and isinstance(p, Path) and p.exists():
                self.path = p
                self.doc = load_doc(self.path, safe=True)
                self.view = self._center_target()
                self._start_local_watch()
        self.mode = "canvas"

    # ---------- viewport math (zoom-to-cursor) ----------

    def _canvas_dimensions(self) -> tuple[int, int]:
        size = shutil.get_terminal_size((120, 36))
        body_h, _, canvas_w = self._layout_dims(size.columns, size.lines, allow_hide=True)
        return max(5, canvas_w), max(5, body_h)

    def _center_target(self) -> View:
        bounds = diagram_bounds(self.doc)
        margin = 2
        w, h = self._canvas_dimensions()
        usable_w = max(1, w - margin * 2)
        usable_h = max(1, h - margin * 2)
        zoom = min(usable_w / max(1, bounds.w), usable_h / max(1, bounds.h), 1.0)
        zoom = max(0.05, zoom)
        cx = bounds.x + bounds.w / 2
        cy = bounds.y + bounds.h / 2
        return View(cx - (w / zoom) / 2, cy - (h / zoom) / 2, zoom)

    def fit(self) -> None:
        self.view = self._center_target()
        self.set_message("fit viewport to diagram bounds", ttl=1.0)

    def zoom(self, factor: float) -> None:
        """Zoom relative to the canvas center.

        The world point currently under the canvas center stays under the
        canvas center after the zoom change. This keeps the diagram
        visually stable instead of drifting toward (0,0).
        """
        w, h = self._canvas_dimensions()
        cx_screen = w / 2.0
        cy_screen = h / 2.0
        # World point currently under the center.
        world_cx = self.view.x + cx_screen / self.view.zoom
        world_cy = self.view.y + cy_screen / self.view.zoom
        new_zoom = max(0.1, min(4.0, self.view.zoom * factor))
        if new_zoom == self.view.zoom:
            return
        new_view_x = world_cx - cx_screen / new_zoom
        new_view_y = world_cy - cy_screen / new_zoom
        self.view = View(new_view_x, new_view_y, new_zoom)

    def animate_center(self) -> None:
        if self.animating:
            return
        self.animating = True
        try:
            start = self.view
            target = self._center_target()
            frames = 14
            for i in range(1, frames + 1):
                t = i / frames
                eased = t * t * (3 - 2 * t)
                self.view = View(
                    start.x + (target.x - start.x) * eased,
                    start.y + (target.y - start.y) * eased,
                    start.zoom + (target.zoom - start.zoom) * eased,
                )
                self.set_message("centering…", ttl=0.4)
                self.draw()
                time.sleep(0.018)
            self.view = target
            self.set_message("centered on diagram", ttl=1.0)
        finally:
            self.animating = False

    # ---------- examples ----------

    def open_selected_example(self) -> None:
        if not self.examples:
            self.set_message("no examples found", ttl=1.5)
            return
        self.path = self.examples[self.example_index]
        self.doc = load_doc(self.path, safe=True)
        self.view = self._center_target()
        self.mode = "canvas"
        self._start_local_watch()
        self.set_message(f"opened {self.path.name}", ttl=1.0)

    def open_example(self, delta: int) -> None:
        if not self.examples:
            self.set_message("no examples found", ttl=1.5)
            return
        self._sync_example_index()
        self.example_index = (self.example_index + delta) % len(self.examples)
        self.open_selected_example()

    # ---------- draw + key I/O ----------

    def _tick(self, speed: float = 8.0) -> int:
        return int((time.monotonic() - self.started_at) * speed)

    def _spinner(self) -> str:
        frames = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
        return frames[self._tick(10.0) % len(frames)]

    def _pulse_code(self) -> str:
        return [CYAN, GREEN, YELLOW, MAGENTA][self._tick(2.0) % 4]

    def _pressure_score(self, doc: dict[str, Any]) -> tuple[int, float]:
        nodes = len(doc.get("nodes", []))
        edges = len(doc.get("edges", []))
        ports = sum(len(n.get("ports", [])) for n in doc.get("nodes", []))
        annotations = len(doc.get("annotations", []))
        groups = len(doc.get("groups", []))
        score = nodes * 9 + edges * 7 + ports * 2 + annotations * 6 + groups * 4
        return score, min(1.0, score / 140.0)

    def _pressure_line(self, doc: dict[str, Any], width: int) -> str:
        score, t = self._pressure_score(doc)
        bar_w = max(10, width)
        filled = max(1, round(t * bar_w))
        empty = max(0, bar_w - filled)
        pen = GREEN if t < 0.45 else YELLOW if t < 0.75 else RED
        label = "pressure"
        if self.color:
            bar = style("█" * filled, True, pen) + style("░" * empty, True, DIM)
        else:
            bar = "█" * filled + "░" * empty
        return f"{label} {bar} {score}"

    def _status_text(self) -> str:
        now = time.monotonic()
        if self.message and now < self.message_until:
            return self.message
        self.message = ""
        return ""

    def draw(self) -> None:
        size = shutil.get_terminal_size((140, 40))
        sys.stdout.write("\033[H\033[2J")
        sys.stdout.write(self.frame(size.columns, size.lines))
        sys.stdout.flush()

    def read_key(self) -> str:
        r, _, _ = select.select([sys.stdin], [], [], 0.12)
        if not r:
            return ""
        ch = sys.stdin.read(1)
        if ch == "\x1b":
            r2, _, _ = select.select([sys.stdin], [], [], 0.01)
            if r2:
                seq = sys.stdin.read(2)
                return {"[A": "up", "[B": "down", "[C": "right", "[D": "left"}.get(seq, ch + seq)
            return "escape"
        if ch in {"\r", "\n"}:
            return "enter"
        return ch

    def cycle_theme(self) -> None:
        i = (THEME_ORDER.index(self.theme) + 1) % len(THEME_ORDER)
        self.theme = THEME_ORDER[i]
        self.set_message(f"theme: {self.theme}", ttl=1.0)

    # ---------- key dispatch ----------

    def handle_key(self, key: str) -> bool:
        if key == "":
            return True
        # Overlays take over key handling.
        if self.overlay == "help":
            self.overlay = None
            return True
        if self.overlay == "command":
            return self._handle_command_key(key)
        if self.overlay == "search":
            return self._handle_search_key(key)
        if self.overlay == "prompt":
            return self._handle_prompt_key(key)
        if self.overlay == "chat":
            return self._handle_chat_key(key)
        if key in {"q", "\x03"}:
            return False
        if key == "?" and not self.overlay:
            self.overlay = "help"
            return True
        if key == ":" and not self.overlay:
            self.overlay = "command"
            self.prompt_buffer = ""
            return True
        if key == "/" and not self.overlay:
            self.overlay = "search"
            self.prompt_buffer = ""
            self.search_index = 0
            return True
        if key == "m" and not self.overlay:
            self.overlay = "chat"
            self.set_message("chat opened", ttl=0.8)
            return True
        if key in {"f"} and self.client_mode:
            self.follow_active = not self.follow_active
            self.set_message(f"follow: {'on' if self.follow_active else 'off'}", ttl=1.0)
            return True
        if key in {"L"} and self.client_mode:
            self.live_reload = not self.live_reload
            self.set_message(f"live reload: {'on' if self.live_reload else 'off'}", ttl=1.0)
            return True
        if key == "W":
            self._create_workspace_prompt()
            return True
        if key == "P":
            self._create_project_prompt()
            return True
        if key == "D":
            self._delete_current()
            return True
        if key == "R":
            self._rename_current()
            return True
        if key == "C":
            self._copy_current_to_project()
            return True
        if key == "u":
            self.refresh_workspace()
            return True
        if self.mode == "dashboard":
            return self._handle_dashboard_key(key)
        return self._handle_canvas_key(key)

    def _handle_command_key(self, key: str) -> bool:
        if key == "escape":
            self.overlay = None
            return True
        if key == "enter":
            cmds = self._commands()
            q = self.prompt_buffer.lower()
            matches = [k for k, label in cmds if q in k.lower() or q in label.lower()]
            self.overlay = None
            if matches:
                return self._run_command(matches[0])
            self.set_message("no command", ttl=1.0)
            return True
        if key in {"\x7f", "\b"}:
            self.prompt_buffer = self.prompt_buffer[:-1]
        elif key == "\x15":
            self.prompt_buffer = ""
        elif len(key) == 1 and key.isprintable():
            self.prompt_buffer += key
        return True

    def _handle_search_key(self, key: str) -> bool:
        if key == "escape":
            self.overlay = None
            self.prompt_buffer = ""
            return True
        if key == "enter":
            matches = self._search(self.prompt_buffer)
            if matches and self.search_index < len(matches):
                result = matches[self.search_index]
                self.overlay = None
                self.prompt_buffer = ""
                self._open_search_result(result)
            else:
                self.overlay = None
            return True
        if key in {"down", "j"}:
            self.search_index = min(self.search_index + 1, max(0, len(self._search(self.prompt_buffer)) - 1))
        elif key in {"up", "k"}:
            self.search_index = max(0, self.search_index - 1)
        elif key in {"\x7f", "\b"}:
            self.prompt_buffer = self.prompt_buffer[:-1]
        elif key == "\x15":
            self.prompt_buffer = ""
        elif len(key) == 1 and key.isprintable():
            self.prompt_buffer += key
        return True

    def _handle_prompt_key(self, key: str) -> bool:
        if key == "escape":
            self.overlay = None
            self.prompt_buffer = ""
            return True
        if key == "enter":
            if self.prompt_choices:
                choice = self.prompt_choices[0][0]
                self.overlay = None
                self.prompt_buffer = ""
                return self._prompt_on_submit(choice) or True
            value = self.prompt_buffer
            self.overlay = None
            self.prompt_buffer = ""
            self._prompt_on_submit(value)
            return True
        if key in {"down", "j"} and self.prompt_choices:
            self.prompt_choices = self.prompt_choices[1:] + self.prompt_choices[:1]
        elif key in {"up", "k"} and self.prompt_choices:
            self.prompt_choices = self.prompt_choices[-1:] + self.prompt_choices[:-1]
        elif key in {"\x7f", "\b"}:
            self.prompt_buffer = self.prompt_buffer[:-1]
        elif key == "\x15":
            self.prompt_buffer = ""
        elif len(key) == 1 and key.isprintable():
            if not self.prompt_choices:
                self.prompt_buffer += key
        return True

    def _handle_chat_key(self, key: str) -> bool:
        if key in {"q", "\x03"}:
            return False
        if key == "escape" or (key == "m" and not self.chat_input):
            self.overlay = None
            self.set_message("chat closed", ttl=0.8)
            return True
        if key == "enter":
            self.submit_chat()
        elif key == "a" and not self.chat_input:
            self.submit_chat("Give me one useful read of this diagram and one next action.")
        elif key in {"\x7f", "\b"}:
            self.chat_input = self.chat_input[:-1]
        elif key == "\x15":
            self.chat_input = ""
        elif len(key) == 1 and key.isprintable():
            self.chat_input += key
        return True

    def _handle_dashboard_key(self, key: str) -> bool:
        if key in {"j", "down"}:
            self.example_index = (self.example_index + 1) % max(1, len(self.examples))
        elif key in {"k", "up"}:
            self.example_index = (self.example_index - 1) % max(1, len(self.examples))
        elif key in {"enter", " "}:
            self.open_selected_example()
        elif key == "a":
            selected = self.examples[self.example_index] if self.examples else self.path
            self.ask_model(load_doc(selected, safe=True), "Give me one useful read of this diagram and one next action.")
        elif key == "t":
            self.cycle_theme()
        elif key == "c":
            self.color = not self.color
        return True

    def _handle_canvas_key(self, key: str) -> bool:
        pan = max(1.0, 4.0 / max(0.25, self.view.zoom))
        if key in {"escape", "tab"}:
            self.mode = "dashboard"
            self._sync_example_index()
            self.set_message("dashboard", ttl=0.8)
        elif key in {"h", "left"}:
            self.view.x -= pan
        elif key in {"l", "right"}:
            self.view.x += pan
        elif key in {"k", "up"}:
            self.view.y -= pan
        elif key in {"j", "down"}:
            self.view.y += pan
        elif key in {"+", "="}:
            self.zoom(1.15)
        elif key in {"-", "_"}:
            self.zoom(1 / 1.15)
        elif key == "0":
            self.view = View()
        elif key == "f":
            if self.client_mode:
                self.follow_active = not self.follow_active
                self.set_message(f"follow: {'on' if self.follow_active else 'off'}", ttl=1.0)
            else:
                self.fit()
        elif key == " ":
            self.animate_center()
        elif key == "t":
            self.cycle_theme()
        elif key == "c":
            self.color = not self.color
        elif key == "g":
            self.legend = not self.legend
        elif key == "o":
            self.ports = not self.ports
        elif key == "b":
            self.sidebar_visible = not self.sidebar_visible
            self.set_message("info rail shown" if self.sidebar_visible else "info rail hidden", ttl=1.0)
        elif key == "x":
            self.crosshair_visible = not self.crosshair_visible
            self.set_message("crosshair on" if self.crosshair_visible else "crosshair off", ttl=1.0)
        elif key == "n":
            self.open_example(1)
        elif key == "p":
            self.open_example(-1)
        elif key == "r":
            self.doc = load_doc(self.path, safe=True)
            self.set_message(f"reloaded {self.path.name}", ttl=1.2)
        elif key == "s":
            self.save_state()
        elif key == "a":
            self.ask_model(self.doc, "Give me one useful read of this diagram and one next action.")
        return True


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Raw ANSI diagram-junky TUI prototype")
    ap.add_argument("diagram", type=Path, nargs="?", help="Path to *.diagram.json")
    ap.add_argument("--example", help="Bundled example name")
    ap.add_argument("--state", type=Path, default=DEFAULT_STATE, help="Viewport/session state file")
    ap.add_argument("--client", action="store_true", help="Attach to a diagram_workspace relic server")
    ap.add_argument("--server-url", default="http://localhost:8127", help="relic server base URL (client mode)")
    ap.add_argument("--provider", default="deepseek", help="Model provider for the embedded pet harness")
    ap.add_argument("--model", default="deepseek-v4-pro", help="Model for the embedded pet harness")
    ap.add_argument("--model-timeout", type=int, default=120, help="Seconds before model ask times out")
    ap.add_argument("--no-color", action="store_true", help="Disable ANSI colors in rendered canvas")
    ap.add_argument("--smoke-render", action="store_true", help="Render one frame and exit for tests/CI")
    args = ap.parse_args(argv)

    app = DiagramTui(
        args.diagram,
        args.example,
        args.state,
        color=not args.no_color,
        client_mode=args.client,
        server_url=args.server_url,
        provider=args.provider,
        model=args.model,
        model_timeout=args.model_timeout,
    )
    if args.smoke_render:
        size = shutil.get_terminal_size((140, 40))
        sys.stdout.write(app.frame(size.columns, size.lines))
        return 0
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
