#!/usr/bin/env python3
"""Raw ANSI diagram-junky TUI prototype.

The shape is closer to Glow/dash.nvim than a bare renderer dump:

- dashboard-first start screen
- focused document list + preview
- structured canvas layout with sidebar/chrome
- smooth space-to-center animation

Still no curses dependency. This remains playground-local and keeps rendering
inside ``diagram_junky.rendering.Renderer`` so the future hub/canvas app can
reuse the same logical renderer.
"""

from __future__ import annotations

import argparse
import json
import re
import select
import shutil
import subprocess
import sys
import termios
import threading
import time
import tty
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib import error as urlerror
from urllib import request as urlrequest

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


def center_text(s: str, width: int) -> str:
    clean = strip_ansi(s)
    left = max(0, (width - len(clean)) // 2)
    return " " * left + s + " " * max(0, width - left - len(clean))


def rule(title: str, width: int, *, color: bool = False, active: bool = False) -> str:
    """Glow-style horizontal section rule. No corners, no vertical chrome."""
    width = max(4, width)
    pen = CYAN if active else DIM
    label = f" {title.strip()} " if title.strip() else ""
    line = label + "─" * max(0, width - visible_len(label))
    line = fit_ansi(line, width)
    return style(line, color, pen)


def panel(title: str, width: int, height: int, body: list[str], *, color: bool = False, active: bool = False) -> list[str]:
    width = max(4, width)
    height = max(1, height)
    # One breathing row after the section rule keeps Glow-style headers from
    # crashing into content while preserving the no-boxes/no-corners chrome.
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


class DiagramTui:
    def __init__(
        self,
        path: Path | None,
        example: str | None,
        state_path: Path,
        color: bool,
        provider: str = "deepseek",
        model: str = "deepseek-v4-pro",
        model_timeout: int = 120,
        server_url: str = "http://localhost:8127",
    ):
        self.state_path = state_path
        self.color = color
        self.provider = provider
        self.model = model
        self.model_timeout = model_timeout
        self.server_url = server_url.rstrip("/")
        self.current_workspace = "default"
        self.current_project = "inbox"
        self.workspaces: list[dict[str, Any]] = []
        self.projects: list[dict[str, Any]] = []
        self.project_diagrams: list[dict[str, Any]] = []
        self.pet_state = "idle"
        self.pet_note = "idle"
        self.model_output = ""
        self.chat_input = ""
        self.chat_history: list[tuple[str, str]] = []
        self.model_thread: threading.Thread | None = None
        self.examples = sorted(EXAMPLES_DIR.glob("*.diagram.json"))
        self.example_index = 0
        self.path = self._resolve_path(path, example)
        self.doc = load_doc(self.path)
        self.view = View()
        self.theme = "neon"
        self.legend = True
        self.ports = False
        self.sidebar_visible = False
        self.chat_visible = False
        self.crosshair_visible = True
        self.mode = "canvas" if path or example else "dashboard"
        self.started_at = time.monotonic()
        self.message = ""
        self.message_until = 0.0
        self._load_state_if_relevant(path, example)
        self._sync_example_index()

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

    def _load_state_if_relevant(self, path: Path | None, example: str | None) -> None:
        if path or example or not self.state_path.exists():
            return
        try:
            raw = json.loads(self.state_path.read_text())
            saved_path = Path(raw.get("path", ""))
            if saved_path.exists():
                self.path = saved_path
                self.doc = load_doc(self.path)
            self.view = View(float(raw.get("x", 0)), float(raw.get("y", 0)), float(raw.get("zoom", 1)))
            self.theme = raw.get("theme", self.theme) if raw.get("theme") in THEMES else self.theme
            self.legend = bool(raw.get("legend", self.legend))
            self.ports = bool(raw.get("ports", self.ports))
            self.sidebar_visible = bool(raw.get("sidebar_visible", self.sidebar_visible))
            self.crosshair_visible = bool(raw.get("crosshair_visible", self.crosshair_visible))
            self.current_workspace = raw.get("current_workspace", self.current_workspace)
            self.current_project = raw.get("current_project", self.current_project)
            self.set_message(f"loaded state from {self.state_path}", ttl=1.2)
        except Exception as e:
            self.set_message(f"state load skipped: {e}", ttl=1.5)

    def run(self) -> None:
        with RawMode():
            sys.stdout.write("\033[?1049h\033[?25l")
            try:
                while True:
                    self.draw()
                    key = self.read_key()
                    if not self.handle_key(key):
                        break
            finally:
                sys.stdout.write("\033[?25h\033[?1049l")
                sys.stdout.flush()

    def frame(self, width: int, height: int) -> str:
        width = max(40, width)
        height = max(16, height)
        if self.chat_visible:
            return self.chat_frame(width, height)
        if self.mode == "dashboard":
            return self.dashboard_frame(width, height)
        return self.canvas_frame(width, height)

    def dashboard_frame(self, width: int, height: int) -> str:
        body_h, left_w, right_w = self.layout_dims(width, height, allow_hide=False)
        header = self.header_lines(width, "dashboard", "docs browser · dash launcher · raw ANSI")

        menu_body = [style("examples", self.color, BOLD, MAGENTA), ""]
        for i, path in enumerate(self.examples):
            try:
                doc = load_doc(path)
            except Exception:
                doc = {"id": path.stem, "title": path.stem, "kind": "broken"}
            label = path.name.removesuffix(".diagram.json")
            prefix = "● " if i == self.example_index else "  "
            row = f"{prefix}{label:<18} {doc.get('kind', 'diagram')}"
            if i == self.example_index:
                row = style(row, self.color, BOLD, CYAN)
            menu_body.append(row)
        menu_body += ["", style("enter/space", self.color, GREEN) + " open", "j/k or arrows select", "t theme · c color · q quit"]

        selected = self.examples[self.example_index] if self.examples else self.path
        selected_doc = load_doc(selected)
        preview = Renderer(
            selected_doc,
            max(10, right_w),
            max(5, body_h - 6),
            color=self.color,
            theme=self.theme,
            view_x=0,
            view_y=0,
            zoom=1,
            legend=False,
            ports=False,
        ).render().splitlines()
        meta = [
            style(selected_doc.get("title", selected.stem), self.color, BOLD, YELLOW),
            f"id: {selected_doc.get('id', selected.stem)}",
            f"nodes: {len(selected_doc.get('nodes', []))} · edges: {len(selected_doc.get('edges', []))}",
            "",
        ]
        preview_body = meta + preview

        left = panel("documents", left_w, body_h, menu_body, color=self.color, active=True)
        right = panel("preview", right_w, body_h, preview_body, color=self.color, active=False)
        rows = header
        for a, b in zip(left, right):
            rows.append(a + " " + b)
        rows.append(style(plain_fit(f" {self.pressure_line(selected_doc, 30)}  {self.status_text()}", width), self.color, DIM))
        rows.append(style(plain_fit(f" {self.example_dial(24)}  a ask · m chat · j/k select · enter open · t theme · c color · q quit", width), self.color, DIM))
        rows = self.overlay_pet(rows[:height], width)
        return "\n".join(rows[:height]) + "\n"

    def canvas_frame(self, width: int, height: int) -> str:
        body_h, side_w, canvas_w = self.layout_dims(width, height, allow_hide=True)
        title = self.doc.get("title", self.doc.get("id", self.path.name))
        header = self.header_lines(width, title, self.path_label())

        sidebar = self.sidebar_lines(side_w, body_h) if self.sidebar_visible else []
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
            rendered = self.with_crosshair(rendered, max(5, canvas_w), max(5, body_h))
        canvas_panel = panel("canvas", canvas_w, body_h, rendered, color=self.color, active=True)
        rows = header
        if self.sidebar_visible:
            left = panel("hub", side_w, body_h, sidebar, color=self.color, active=False)
            for a, b in zip(left, canvas_panel):
                rows.append(a + " " + b)
        else:
            rows.extend(canvas_panel)
        rows.append(style(plain_fit(f" {self.pressure_line(self.doc, 30)}  {self.status_text()}", width), self.color, DIM))
        rows.append(style(plain_fit(" a ask · m chat · space center · x crosshair · b rail · f fit · hjkl/arrows pan · +/- zoom · esc dashboard · q quit", width), self.color, DIM))
        rows = self.overlay_pet(rows[:height], width)
        return "\n".join(rows[:height]) + "\n"

    def layout_dims(self, width: int, height: int, *, allow_hide: bool = False) -> tuple[int, int, int]:
        # Stable geometry across dashboard/canvas: selecting a diagram should not
        # make the whole app jump. Header is always exactly two rows.
        header_h = 2
        footer_h = 2
        body_h = max(8, height - header_h - footer_h)
        if not allow_hide:
            side_w = min(42, max(30, width // 3))
        else:
            side_w = min(24, max(20, width // 5)) if self.sidebar_visible else 0
        main_w = max(20, width - side_w - (1 if side_w else 0))
        return body_h, side_w, main_w

    def chat_frame(self, width: int, height: int) -> str:
        body_h = max(8, height - 6)
        title = "harness chat"
        header = self.header_lines(width, title, f"{self.provider}/{self.model}")
        transcript = self.chat_transcript(width - 2, body_h)
        input_line = "> " + self.chat_input
        if self.pet_state == "thinking":
            input_line += "  " + self.spinner()
        rows = header
        rows.extend(panel("transcript", width, body_h, transcript, color=self.color, active=True))
        rows.append(rule("prompt", width, color=self.color, active=True))
        rows.append(fit_ansi(style(input_line[-max(1, width - 1):], self.color, BOLD, CYAN), width))
        rows.append(style(plain_fit(f" {self.pressure_line(self.doc, 30)}  {self.status_text()}", width), self.color, DIM))
        rows.append(style(plain_fit(" enter send · a quick ask · m/esc close · backspace edit · q quit", width), self.color, DIM))
        return "\n".join(rows[:height]) + "\n"

    def path_label(self) -> str:
        rel = self.path.relative_to(ROOT) if self.path.is_relative_to(ROOT) else self.path
        return str(rel)

    def header_lines(self, width: int, title: str, subtitle: str) -> list[str]:
        # Max height 2. Row 1 = identity/title. Row 2 = state + useful dials.
        left = f" {self.spinner()} diagram-junky · {title}"
        row1 = style(plain_fit(left, width), self.color, BOLD, self.pulse_code())
        status = (
            f" {subtitle} · theme={self.theme} · zoom={self.view.zoom:.2f} "
            f"{self.zoom_dial(18)} · view=({self.view.x:.1f},{self.view.y:.1f})"
        )
        row2 = style(plain_fit(status, width), self.color, DIM)
        return [row1, row2]

    def zoom_dial(self, width: int) -> str:
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

    def example_dial(self, width: int) -> str:
        width = max(8, width)
        total = max(1, len(self.examples))
        pos = 0 if total == 1 else round((self.example_index / (total - 1)) * (width - 1))
        chars = ["─"] * width
        chars[pos] = "●"
        return "".join(chars) + f" {self.example_index + 1}/{total}"

    def wrap_plain(self, text: str, width: int, max_lines: int) -> list[str]:
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

    def chat_transcript(self, width: int, height: int) -> list[str]:
        lines: list[str] = []
        lines.append(style("pet", self.color, BOLD, MAGENTA))
        pet = self.pet_lines()
        lines.extend(pet[:5])
        lines.append("")
        lines.append(style(f"model {self.provider}/{self.model}", self.color, DIM))
        lines.append(style(f"diagram {self.doc.get('id', self.path.stem)} · {self.current_workspace}/{self.current_project}", self.color, DIM))
        lines.append("")
        if not self.chat_history:
            lines.extend([
                style("no messages yet", self.color, DIM),
                "type a prompt below, or press a for a quick read",
            ])
        else:
            for role, text in self.chat_history[-8:]:
                label = style("you", self.color, BOLD, CYAN) if role == "you" else style("pet", self.color, BOLD, MAGENTA)
                lines.append(label)
                lines.extend("  " + line for line in self.wrap_plain(text, max(8, width - 2), 8))
                lines.append("")
        return [fit_ansi(line, width) for line in lines[-height:]]

    def server_call(self, endpoint: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        data = json.dumps(payload or {}).encode()
        req = urlrequest.Request(
            self.server_url + endpoint,
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urlrequest.urlopen(req, timeout=2.0) as resp:
                return json.loads(resp.read().decode())
        except (urlerror.URLError, TimeoutError, json.JSONDecodeError) as e:
            raise RuntimeError(f"workspace server unavailable: {e}") from e

    def refresh_workspace(self) -> None:
        try:
            ws = self.server_call("/workspace/list")
            self.workspaces = ws.get("workspaces", [])
            pr = self.server_call("/project/list", {"workspace": self.current_workspace})
            self.projects = pr.get("projects", [])
            dg = self.server_call("/project/diagrams", {"workspace": self.current_workspace, "project": self.current_project})
            self.project_diagrams = dg.get("diagrams", [])
            self.set_message("workspace refreshed", ttl=1.0)
        except Exception as e:
            self.set_message(str(e), ttl=3.0)

    def create_workspace(self) -> None:
        wid = f"ws-{int(time.time())}"
        try:
            self.server_call("/workspace/create", {"workspace": wid, "title": wid})
            self.current_workspace = wid
            self.current_project = "inbox"
            self.server_call("/project/create", {"workspace": wid, "project": "inbox", "title": "inbox"})
            self.refresh_workspace()
            self.set_message(f"workspace created: {wid}", ttl=2.0)
        except Exception as e:
            self.set_message(str(e), ttl=3.0)

    def create_project(self) -> None:
        pid = f"project-{int(time.time())}"
        try:
            self.server_call("/project/create", {"workspace": self.current_workspace, "project": pid, "title": pid})
            self.current_project = pid
            self.refresh_workspace()
            self.set_message(f"project created: {pid}", ttl=2.0)
        except Exception as e:
            self.set_message(str(e), ttl=3.0)

    def copy_current_to_project(self) -> None:
        try:
            self.server_call(
                "/project/copy",
                {
                    "workspace": self.current_workspace,
                    "project": self.current_project,
                    "id": self.doc.get("id", self.path.stem),
                    "document": self.doc,
                },
            )
            self.refresh_workspace()
            self.set_message(f"copied {self.doc.get('id', self.path.stem)} → {self.current_workspace}/{self.current_project}", ttl=2.0)
        except Exception as e:
            self.set_message(str(e), ttl=3.0)

    def sidebar_lines(self, width: int, height: int) -> list[str]:
        # Optional info rail, not a second menu. Keep it quiet.
        lines = [
            style("workspace", self.color, BOLD, MAGENTA),
            self.current_workspace,
            f"project {self.current_project}",
            f"diagrams {len(self.project_diagrams)}",
            "W new ws · P project",
            "C copy · u refresh",
            "",
            style("info", self.color, BOLD, MAGENTA),
            self.doc.get("id", self.path.stem),
            f"nodes  {len(self.doc.get('nodes', []))}",
            f"edges  {len(self.doc.get('edges', []))}",
            "",
            style("zoom", self.color, BOLD, MAGENTA),
            f"{self.view.zoom:.2f}",
            self.zoom_dial(max(8, width)),
            "",
            style("flags", self.color, BOLD, MAGENTA),
            f"ports   {'on' if self.ports else 'off'}",
            f"legend  {'on' if self.legend else 'off'}",
            f"cross   {'on' if self.crosshair_visible else 'off'}",
            "",
            style("b hides rail", self.color, DIM),
        ]
        return [fit_ansi(line, width) for line in lines[:height]]

    def replace_visible_char(self, line: str, col: int, ch: str) -> str:
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

    def with_crosshair(self, lines: list[str], width: int, height: int) -> list[str]:
        out = list(lines[:height])
        while len(out) < height:
            out.append("")
        cx = max(0, width // 2)
        cy = max(0, height // 2)
        mark = style("┼", self.color, BOLD, MAGENTA)
        arm = style("·", self.color, DIM)
        for dx in (-2, -1, 1, 2):
            out[cy] = self.replace_visible_char(out[cy], cx + dx, arm)
        for dy in (-2, -1, 1, 2):
            y = cy + dy
            if 0 <= y < len(out):
                out[y] = self.replace_visible_char(out[y], cx, arm)
        out[cy] = self.replace_visible_char(out[cy], cx, mark)
        return out

    def pet_lines(self) -> list[str]:
        if self.pet_state == "thinking":
            eyes = ["o.o", "O.o", "o.O", "O.O"][self.tick(7.0) % 4]
            return [" /\\ ", f"({eyes})", " /|\\", " / \\", " ask "]
        if self.pet_state == "error":
            return [" /\\ ", "(x.x)", " /|\\", " / \\", " err "]
        if self.pet_state == "done":
            return [" /\\ ", "(^.^)", " /|\\", " / \\", " ok  "]
        return [" /\\ ", "(·.·)", " /|\\", " / \\", " ai  "]

    def overlay_pet(self, rows: list[str], width: int) -> list[str]:
        if width < 48 or len(rows) < 8:
            return rows
        pet = self.pet_lines()
        start = max(2, len(rows) - 7)
        col = max(0, width - 7)
        out = list(rows)
        for i, line in enumerate(pet):
            y = start + i
            if y >= len(out):
                break
            out[y] = fit_ansi(out[y], col) + style(line, self.color, DIM)
        return out

    def submit_chat(self, text: str | None = None) -> None:
        prompt = (text if text is not None else self.chat_input).strip()
        if not prompt:
            prompt = "Give me one useful read of this diagram and one next action."
        self.chat_input = ""
        self.chat_history.append(("you", prompt))
        self.ask_model(self.doc, user_text=prompt)

    def clean_model_text(self, raw: str) -> str:
        text = strip_ansi(raw).strip()
        m = re.search(r"<response[^>]*>(.*?)</response>", text, re.DOTALL)
        if m:
            return m.group(1).strip()
        return text

    def model_prompt(self, doc: dict[str, Any], user_text: str = "") -> str:
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
            f"pressure: {self.pressure_score(doc)[0]}\n"
            f"workspace: {self.current_workspace}\n"
            f"project: {self.current_project}\n"
            "You may propose workspace/project operations, but do not assume they are applied until confirmed.\n"
        )

    def ask_model(self, doc: dict[str, Any], user_text: str = "") -> None:
        if self.model_thread and self.model_thread.is_alive():
            self.set_message("model already thinking", ttl=1.0)
            return
        self.pet_state = "thinking"
        self.pet_note = f"{self.provider}/{self.model}"
        self.set_message(f"asking {self.model}", ttl=1.2)

        def run() -> None:
            try:
                exe = PROJECT_ROOT / "cortex-mk3"
                if not exe.exists():
                    raise FileNotFoundError(str(exe))
                manifest = ROOT / "manifests" / "agents" / "diagram-junky" / "agent.yml"
                cmd = [
                    str(exe),
                    "--provider",
                    self.provider,
                    "--model",
                    self.model,
                    "--raw",
                    "--manifest",
                    str(manifest),
                    "run",
                    "-p",
                    self.model_prompt(doc, user_text),
                ]
                proc = subprocess.run(cmd, cwd=PROJECT_ROOT, text=True, capture_output=True, timeout=self.model_timeout)
                if proc.returncode != 0:
                    raise RuntimeError((proc.stderr or proc.stdout).strip()[:240])
                self.model_output = self.clean_model_text(proc.stdout)[-800:]
                self.chat_history.append(("pet", self.model_output or "model done"))
                self.pet_state = "done"
                self.set_message("model replied", ttl=2.0)
            except Exception as e:
                self.pet_state = "error"
                self.chat_history.append(("pet", f"error: {e}"))
                self.set_message(f"model error: {e}", ttl=4.0)

        self.model_thread = threading.Thread(target=run, daemon=True)
        self.model_thread.start()

    def set_message(self, text: str, *, ttl: float = 1.2) -> None:
        self.message = text
        self.message_until = time.monotonic() + ttl

    def status_text(self) -> str:
        now = time.monotonic()
        if self.message and now < self.message_until:
            return self.message
        self.message = ""
        return ""

    def pressure_score(self, doc: dict[str, Any]) -> tuple[int, float]:
        nodes = len(doc.get("nodes", []))
        edges = len(doc.get("edges", []))
        ports = sum(len(n.get("ports", [])) for n in doc.get("nodes", []))
        annotations = len(doc.get("annotations", []))
        groups = len(doc.get("groups", []))
        score = nodes * 9 + edges * 7 + ports * 2 + annotations * 6 + groups * 4
        return score, min(1.0, score / 140.0)

    def pressure_line(self, doc: dict[str, Any], width: int) -> str:
        score, t = self.pressure_score(doc)
        bar_w = max(10, width)
        filled = max(1, round(t * bar_w))
        empty = max(0, bar_w - filled)
        pen = GREEN if t < 0.45 else YELLOW if t < 0.75 else RED
        label = "pressure"
        bar = "█" * filled + "░" * empty
        if self.color:
            bar = style("█" * filled, True, pen) + style("░" * empty, True, DIM)
        return f"{label} {bar} {score}"

    def tick(self, speed: float = 8.0) -> int:
        return int((time.monotonic() - self.started_at) * speed)

    def spinner(self) -> str:
        frames = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
        return frames[self.tick(10.0) % len(frames)]

    def dots(self, *, reverse: bool = False) -> str:
        frames = ["·  ", "·· ", "···", " ··", "  ·", "   "]
        i = self.tick(5.0) % len(frames)
        s = frames[-i - 1] if reverse else frames[i]
        return style(s, self.color, DIM)

    def pulse_code(self) -> str:
        return [CYAN, GREEN, YELLOW, MAGENTA][self.tick(2.0) % 4]

    def breath_bar(self, width: int) -> str:
        width = max(6, width)
        phase = self.tick(6.0) % (width * 2)
        pos = phase if phase < width else width * 2 - phase - 1
        chars = ["─"] * width
        for dx, ch in [(-1, "·"), (0, "●"), (1, "·")]:
            j = pos + dx
            if 0 <= j < width:
                chars[j] = ch
        return style("".join(chars), self.color, DIM)

    def draw(self) -> None:
        size = shutil.get_terminal_size((120, 36))
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

    def handle_key(self, key: str) -> bool:
        if key == "":
            return True
        if self.chat_visible:
            if key == "escape" or (key == "m" and not self.chat_input):
                self.chat_visible = False
                self.set_message("chat closed", ttl=0.8)
            elif key == "enter":
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
        if key in {"q", "\x03"}:
            return False
        if key == "m":
            self.chat_visible = True
            self.set_message("chat opened", ttl=0.8)
            return True
        if key == "u":
            self.refresh_workspace()
            return True
        if key == "W":
            self.create_workspace()
            return True
        if key == "P":
            self.create_project()
            return True
        if key == "C":
            self.copy_current_to_project()
            return True
        if self.mode == "dashboard":
            return self.handle_dashboard_key(key)
        return self.handle_canvas_key(key)

    def handle_dashboard_key(self, key: str) -> bool:
        if key in {"j", "down"}:
            self.example_index = (self.example_index + 1) % max(1, len(self.examples))
        elif key in {"k", "up"}:
            self.example_index = (self.example_index - 1) % max(1, len(self.examples))
        elif key in {"enter", " "}:
            self.open_selected_example()
        elif key == "a":
            selected = self.examples[self.example_index] if self.examples else self.path
            self.ask_model(load_doc(selected), "Give me one useful read of this diagram and one next action.")
        elif key == "t":
            self.cycle_theme()
        elif key == "c":
            self.color = not self.color
        elif key == "?":
            self.set_message("dashboard keys: j/k select · enter/space open · t theme · c color · q quit", ttl=3.0)
        return True

    def handle_canvas_key(self, key: str) -> bool:
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
            self.view.zoom = min(4.0, self.view.zoom * 1.15)
        elif key in {"-", "_"}:
            self.view.zoom = max(0.1, self.view.zoom / 1.15)
        elif key == "0":
            self.view = View()
        elif key == "f":
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
            self.doc = load_doc(self.path)
            self.set_message(f"reloaded {self.path.name}", ttl=1.2)
        elif key == "s":
            self.save_state()
        elif key == "a":
            self.ask_model(self.doc, "Give me one useful read of this diagram and one next action.")
        elif key == "?":
            self.set_message("keys: a ask model · space center · x crosshair · b rail · esc dashboard · arrows/hjkl pan · +/- zoom · f fit · t/c/g/o toggles · r reload · s save", ttl=3.0)
        return True

    def cycle_theme(self) -> None:
        i = (THEME_ORDER.index(self.theme) + 1) % len(THEME_ORDER)
        self.theme = THEME_ORDER[i]
        self.set_message(f"theme: {self.theme}", ttl=1.0)

    def canvas_dimensions(self) -> tuple[int, int]:
        size = shutil.get_terminal_size((120, 36))
        body_h, _, canvas_w = self.layout_dims(size.columns, size.lines, allow_hide=True)
        return max(5, canvas_w), max(5, body_h)

    def center_target(self) -> View:
        # Renderer coordinates are logical viewport coordinates: screen=(p-view)*zoom.
        # True centering means diagram center maps to canvas center, not simply
        # placing the top-left bound at a margin.
        w, h = self.canvas_dimensions()
        bounds = diagram_bounds(self.doc)
        margin = 2
        usable_w = max(1, w - margin * 2)
        usable_h = max(1, h - margin * 2)
        zoom = min(usable_w / max(1, bounds.w), usable_h / max(1, bounds.h), 1.0)
        zoom = max(0.05, zoom)
        cx = bounds.x + bounds.w / 2
        cy = bounds.y + bounds.h / 2
        return View(cx - (w / zoom) / 2, cy - (h / zoom) / 2, zoom)

    def fit(self) -> None:
        self.view = self.center_target()
        self.set_message("fit viewport to diagram bounds", ttl=1.0)

    def animate_center(self) -> None:
        start = self.view
        target = self.center_target()
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

    def open_selected_example(self) -> None:
        if not self.examples:
            self.set_message("no examples found", ttl=1.5)
            return
        self.path = self.examples[self.example_index]
        self.doc = load_doc(self.path)
        self.view = self.center_target()
        self.mode = "canvas"
        self.set_message(f"opened {self.path.name}", ttl=1.0)

    def open_example(self, delta: int) -> None:
        if not self.examples:
            self.set_message("no examples found", ttl=1.5)
            return
        self._sync_example_index()
        self.example_index = (self.example_index + delta) % len(self.examples)
        self.open_selected_example()

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
            "saved_at": time.time(),
        }
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(payload, indent=2))
        self.set_message(f"saved state to {self.state_path}", ttl=1.5)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Raw ANSI diagram-junky TUI prototype")
    ap.add_argument("diagram", type=Path, nargs="?", help="Path to *.diagram.json")
    ap.add_argument("--example", help="Bundled example name")
    ap.add_argument("--state", type=Path, default=DEFAULT_STATE, help="Viewport/session state file")
    ap.add_argument("--provider", default="deepseek", help="Model provider for the embedded pet harness")
    ap.add_argument("--model", default="deepseek-v4-pro", help="Model for the embedded pet harness")
    ap.add_argument("--model-timeout", type=int, default=120, help="Seconds before model ask times out")
    ap.add_argument("--server-url", default="http://localhost:8127", help="diagram_workspace server/relic URL")
    ap.add_argument("--no-color", action="store_true", help="Disable ANSI colors in rendered canvas")
    ap.add_argument("--smoke-render", action="store_true", help="Render one frame and exit for tests/CI")
    args = ap.parse_args(argv)

    app = DiagramTui(
        args.diagram,
        args.example,
        args.state,
        color=not args.no_color,
        provider=args.provider,
        model=args.model,
        model_timeout=args.model_timeout,
        server_url=args.server_url,
    )
    if args.smoke_render:
        size = shutil.get_terminal_size((100, 28))
        sys.stdout.write(app.frame(size.columns, size.lines))
        return 0
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
