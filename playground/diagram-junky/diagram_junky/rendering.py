#!/usr/bin/env python3
"""diagram-junky CLI renderer.

Prototype renderer for `diagram.document.v0` files. The important split is:

    Diagram JSON -> logical scene -> styled character canvas

The TUI can keep the same logical scene and swap the final canvas target later.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import sys
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Iterable, Literal


@dataclass(frozen=True)
class Pt:
    x: float
    y: float


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    w: float
    h: float


@dataclass(frozen=True)
class Pen:
    fg: str | None = None
    bg: str | None = None
    bold: bool = False
    dim: bool = False
    italic: bool = False
    inverse: bool = False


@dataclass(frozen=True)
class Cell:
    ch: str = " "
    pen: Pen = Pen()


BORDERS = {
    "square": {"h": "─", "v": "│", "tl": "┌", "tr": "┐", "bl": "└", "br": "┘", "cross": "┼"},
    "rounded": {"h": "─", "v": "│", "tl": "╭", "tr": "╮", "bl": "╰", "br": "╯", "cross": "┼"},
    "double": {"h": "═", "v": "║", "tl": "╔", "tr": "╗", "bl": "╚", "br": "╝", "cross": "╬"},
    "heavy": {"h": "━", "v": "┃", "tl": "┏", "tr": "┓", "bl": "┗", "br": "┛", "cross": "╋"},
    "dotted": {"h": "┄", "v": "┆", "tl": "┌", "tr": "┐", "bl": "└", "br": "┘", "cross": "┼"},
    "ascii": {"h": "-", "v": "|", "tl": "+", "tr": "+", "bl": "+", "br": "+", "cross": "+"},
}

ARROWS = {
    "unicode": {"r": "▶", "l": "◀", "u": "▲", "d": "▼", "dot": "•", "diag_a": "╱", "diag_b": "╲"},
    "ascii": {"r": ">", "l": "<", "u": "^", "d": "v", "dot": "*", "diag_a": "/", "diag_b": "\\"},
}

ANSI_FG = {
    "black": 30,
    "red": 31,
    "green": 32,
    "yellow": 33,
    "blue": 34,
    "magenta": 35,
    "cyan": 36,
    "white": 37,
    "gray": 90,
    "grey": 90,
    "bright_black": 90,
    "bright_red": 91,
    "bright_green": 92,
    "bright_yellow": 93,
    "bright_blue": 94,
    "bright_magenta": 95,
    "bright_cyan": 96,
    "bright_white": 97,
}

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES_DIR = ROOT / "examples"
DOC_SCHEMA = ROOT / "schemas" / "diagram-document.schema.json"

PRESETS = {
    "compact": {"width": 100, "height": 30, "fit": True, "theme": "default"},
    "wide": {"width": 150, "height": 42, "theme": "default"},
    "neon": {"width": 120, "height": 36, "theme": "neon", "legend": True},
    "poster": {"width": 160, "height": 48, "theme": "neon", "legend": True, "ports": True},
}

THEMES: dict[str, dict[str, Pen]] = {
    "default": {
        "text": Pen("white"),
        "muted": Pen("gray"),
        "group": Pen("blue", dim=True),
        "edge": Pen("gray"),
        "flow": Pen("cyan"),
        "input": Pen("green"),
        "result": Pen("yellow"),
        "call": Pen("magenta"),
        "node": Pen("white", bold=True),
        "process": Pen("cyan", bold=True),
        "terminator": Pen("green", bold=True),
        "decision": Pen("yellow", bold=True),
        "external": Pen("magenta", bold=True),
        "collection": Pen("blue", bold=True),
        "actor": Pen("green", bold=True),
        "annotation": Pen("yellow"),
        "warning": Pen("red", bold=True),
    },
    "mono": {
        "text": Pen(),
        "muted": Pen(dim=True),
        "group": Pen(dim=True),
        "edge": Pen(),
        "flow": Pen(),
        "input": Pen(),
        "result": Pen(),
        "call": Pen(),
        "node": Pen(bold=True),
        "process": Pen(bold=True),
        "terminator": Pen(bold=True),
        "decision": Pen(bold=True),
        "external": Pen(bold=True),
        "collection": Pen(bold=True),
        "actor": Pen(bold=True),
        "annotation": Pen(),
        "warning": Pen(bold=True),
    },
    "neon": {
        "text": Pen("bright_white"),
        "muted": Pen("bright_black"),
        "group": Pen("bright_blue", dim=True),
        "edge": Pen("bright_black"),
        "flow": Pen("bright_cyan", bold=True),
        "input": Pen("bright_green", bold=True),
        "result": Pen("bright_yellow", bold=True),
        "call": Pen("bright_magenta", bold=True),
        "node": Pen("bright_white", bold=True),
        "process": Pen("bright_cyan", bold=True),
        "terminator": Pen("bright_green", bold=True),
        "decision": Pen("bright_yellow", bold=True),
        "external": Pen("bright_magenta", bold=True),
        "collection": Pen("bright_blue", bold=True),
        "actor": Pen("bright_green", bold=True),
        "annotation": Pen("bright_yellow"),
        "warning": Pen("bright_red", bold=True),
    },
}


def sgr(pen: Pen) -> str:
    codes: list[str] = []
    if pen.bold:
        codes.append("1")
    if pen.dim:
        codes.append("2")
    if pen.italic:
        codes.append("3")
    if pen.inverse:
        codes.append("7")
    if pen.fg in ANSI_FG:
        codes.append(str(ANSI_FG[pen.fg]))
    if pen.bg in ANSI_FG:
        codes.append(str(ANSI_FG[pen.bg] + 10))
    return f"\033[{';'.join(codes)}m" if codes else ""


def text_width(s: str) -> int:
    # Good enough for this playground: ANSI is added after layout; examples are
    # plain-width labels. TUI can swap in wcwidth later.
    return len(s)


def fit_text(s: str, width: int, ellipsis: str = "…") -> str:
    if width <= 0:
        return ""
    if text_width(s) <= width:
        return s
    if width == 1:
        return ellipsis if ellipsis != "..." else "."
    return s[: max(0, width - text_width(ellipsis))] + ellipsis


class Canvas:
    def __init__(self, width: int, height: int, ascii_mode: bool, color: bool):
        self.width = max(1, width)
        self.height = max(1, height)
        self.ascii_mode = ascii_mode
        self.color = color
        self.border = BORDERS["ascii"] if ascii_mode else BORDERS["square"]
        self.arrow = ARROWS["ascii"] if ascii_mode else ARROWS["unicode"]
        self.grid = [[Cell() for _ in range(self.width)] for _ in range(self.height)]

    def put(self, x: int, y: int, ch: str, pen: Pen = Pen(), force: bool = False) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            old = self.grid[y][x]
            self.grid[y][x] = self._merge(old, Cell(ch, pen), force)

    def text(self, x: int, y: int, s: str, pen: Pen = Pen(), max_width: int | None = None) -> None:
        if y < 0 or y >= self.height:
            return
        if max_width is not None:
            s = fit_text(s, max(0, max_width), "..." if self.ascii_mode else "…")
        for i, ch in enumerate(s):
            if 0 <= x + i < self.width:
                self.grid[y][x + i] = Cell(ch, pen)

    def centered(self, x: int, y: int, w: int, s: str, pen: Pen) -> None:
        s = fit_text(s, max(0, w), "..." if self.ascii_mode else "…")
        self.text(x + max(0, (w - text_width(s)) // 2), y, s, pen, max_width=w)

    def hline(self, x1: int, x2: int, y: int, pen: Pen, style: str = "solid") -> None:
        if y < 0 or y >= self.height:
            return
        a, b = sorted((x1, x2))
        for i, x in enumerate(range(a, b + 1)):
            if self._draw_at(i, style):
                self.put(x, y, self._line_char("h", style), pen)

    def vline(self, x: int, y1: int, y2: int, pen: Pen, style: str = "solid") -> None:
        if x < 0 or x >= self.width:
            return
        a, b = sorted((y1, y2))
        for i, y in enumerate(range(a, b + 1)):
            if self._draw_at(i, style):
                self.put(x, y, self._line_char("v", style), pen)

    def box(self, x: int, y: int, w: int, h: int, label: str = "", pen: Pen = Pen(), border: str = "square") -> None:
        if w < 2 or h < 2:
            return
        b = self._border(border)
        self.hline(x + 1, x + w - 2, y, pen, border if border == "dotted" else "solid")
        self.hline(x + 1, x + w - 2, y + h - 1, pen, border if border == "dotted" else "solid")
        self.vline(x, y + 1, y + h - 2, pen, border if border == "dotted" else "solid")
        self.vline(x + w - 1, y + 1, y + h - 2, pen, border if border == "dotted" else "solid")
        self.put(x, y, b["tl"], pen, force=True)
        self.put(x + w - 1, y, b["tr"], pen, force=True)
        self.put(x, y + h - 1, b["bl"], pen, force=True)
        self.put(x + w - 1, y + h - 1, b["br"], pen, force=True)
        if label:
            label_text = fit_text(label, max(0, w - 6), "..." if self.ascii_mode else "…")
            self.text(x + 2, y, f" {label_text} ", replace(pen, bold=True), max_width=max(0, w - 4))

    def diamond(self, cx: int, cy: int, w: int, h: int, label: str, pen: Pen) -> None:
        # Draw a sparse diamond directly instead of via diagonal polylines.
        # The polyline version looked noisy in terminal cells (double-thick
        # slopes from rounding). This keeps decision nodes crisp.
        rx = max(2, w // 2)
        ry = max(2, h // 2)
        if self.ascii_mode:
            tl, tr, bl, br = "/", "\\", "\\", "/"
        else:
            tl, tr, bl, br = "╱", "╲", "╲", "╱"
        for dy in range(-ry, ry + 1):
            y = cy + dy
            span = round(rx * (1 - abs(dy) / max(1, ry)))
            if span <= 0:
                self.put(cx, y, self.arrow["dot"], pen, force=True)
                continue
            left = cx - span
            right = cx + span
            self.put(left, y, tl if dy < 0 else bl, pen, force=True)
            self.put(right, y, tr if dy < 0 else br, pen, force=True)
        self.centered(cx - rx + 1, cy, max(1, rx * 2 - 1), label, replace(pen, bold=True))

    def cylinder(self, x: int, y: int, w: int, h: int, label: str, pen: Pen) -> None:
        if w < 4 or h < 3:
            self.box(x, y, w, h, label, pen)
            return
        left, right = ("(", ")") if self.ascii_mode else ("╭", "╮")
        lower_left, lower_right = ("(", ")") if self.ascii_mode else ("╰", "╯")
        self.hline(x + 2, x + w - 3, y, pen)
        self.put(x + 1, y, left, pen, force=True)
        self.put(x + w - 2, y, right, pen, force=True)
        self.vline(x, y + 1, y + h - 2, pen)
        self.vline(x + w - 1, y + 1, y + h - 2, pen)
        if h >= 5:
            self.hline(x + 2, x + w - 3, y + 2, pen)
            self.put(x + 1, y + 2, lower_left, pen, force=True)
            self.put(x + w - 2, y + 2, lower_right, pen, force=True)
        self.hline(x + 2, x + w - 3, y + h - 1, pen)
        self.put(x + 1, y + h - 1, lower_left, pen, force=True)
        self.put(x + w - 2, y + h - 1, lower_right, pen, force=True)
        self.centered(x + 1, y + max(2, h // 2), w - 2, label, replace(pen, bold=True))

    def polyline(self, pts: list[tuple[int, int]], pen: Pen, arrow_end: bool = False, line: str = "solid") -> None:
        if len(pts) < 2:
            return
        for a, b in zip(pts, pts[1:]):
            self._segment(a, b, pen, line)
        for prev, cur, nxt in zip(pts, pts[1:], pts[2:]):
            corner = self._corner(prev, cur, nxt)
            if corner:
                self.put(cur[0], cur[1], corner, pen, force=True)
        if arrow_end:
            self._arrow(pts[-2], pts[-1], pen)

    def render(self) -> str:
        lines: list[str] = []
        reset = "\033[0m"
        for row in self.grid:
            raw_len = len(row)
            while raw_len > 0 and row[raw_len - 1].ch == " ":
                raw_len -= 1
            current = Pen()
            line = ""
            for cell in row[:raw_len]:
                if self.color and cell.pen != current:
                    line += reset
                    seq = sgr(cell.pen)
                    if seq:
                        line += seq
                    current = cell.pen
                line += cell.ch
            if self.color and current != Pen():
                line += reset
            lines.append(line)
        return "\n".join(lines).rstrip() + "\n"

    def _segment(self, a: tuple[int, int], b: tuple[int, int], pen: Pen, line: str) -> None:
        x1, y1 = a
        x2, y2 = b
        if x1 == x2:
            self.vline(x1, y1, y2, pen, line)
            return
        if y1 == y2:
            self.hline(x1, x2, y1, pen, line)
            return

        steps = max(abs(x2 - x1), abs(y2 - y1))
        for i in range(steps + 1):
            if not self._draw_at(i, line):
                continue
            t = i / max(1, steps)
            x = round(x1 + (x2 - x1) * t)
            y = round(y1 + (y2 - y1) * t)
            ch = self.arrow["diag_b"] if (x2 - x1) * (y2 - y1) >= 0 else self.arrow["diag_a"]
            self.put(x, y, ch, pen)

    def _arrow(self, a: tuple[int, int], b: tuple[int, int], pen: Pen) -> None:
        dx = b[0] - a[0]
        dy = b[1] - a[1]
        if abs(dx) >= abs(dy):
            self.put(b[0], b[1], self.arrow["r"] if dx >= 0 else self.arrow["l"], pen, force=True)
        else:
            self.put(b[0], b[1], self.arrow["d"] if dy >= 0 else self.arrow["u"], pen, force=True)

    def _corner(self, prev: tuple[int, int], cur: tuple[int, int], nxt: tuple[int, int]) -> str:
        if self.ascii_mode:
            return "+"
        dirs: set[str] = set()
        if prev[0] < cur[0] or nxt[0] < cur[0]:
            dirs.add("l")
        if prev[0] > cur[0] or nxt[0] > cur[0]:
            dirs.add("r")
        if prev[1] < cur[1] or nxt[1] < cur[1]:
            dirs.add("u")
        if prev[1] > cur[1] or nxt[1] > cur[1]:
            dirs.add("d")
        return {
            frozenset({"r", "d"}): "┌",
            frozenset({"l", "d"}): "┐",
            frozenset({"r", "u"}): "└",
            frozenset({"l", "u"}): "┘",
            frozenset({"l", "r"}): "─",
            frozenset({"u", "d"}): "│",
        }.get(frozenset(dirs), "┼")

    def _merge(self, old: Cell, new: Cell, force: bool) -> Cell:
        if force or old.ch == " " or old.ch == new.ch:
            return new
        line_chars = {"─", "│", "━", "┃", "═", "║", "┄", "┆", "-", "|", "/", "\\", "•", "*"}
        if old.ch in line_chars and new.ch in line_chars and old.ch != new.ch:
            return Cell(self._border("square")["cross"], new.pen)
        if new.ch not in line_chars:
            return new
        return old

    def _border(self, name: str) -> dict[str, str]:
        if self.ascii_mode:
            return BORDERS["ascii"]
        return BORDERS.get(name, BORDERS["square"])

    def _line_char(self, axis: Literal["h", "v"], style: str) -> str:
        if self.ascii_mode:
            return "-" if axis == "h" else "|"
        if style == "dotted":
            return "┄" if axis == "h" else "┆"
        if style == "heavy":
            return "━" if axis == "h" else "┃"
        if style == "double":
            return "═" if axis == "h" else "║"
        return "─" if axis == "h" else "│"

    def _draw_at(self, i: int, style: str) -> bool:
        if style == "dashed":
            return i % 6 < 4
        if style == "dotted":
            return i % 2 == 0
        return True


class Renderer:
    def __init__(
        self,
        doc: dict[str, Any],
        width: int,
        height: int,
        ascii_mode: bool = False,
        color: bool = False,
        theme: str = "default",
        view_x: float | None = None,
        view_y: float | None = None,
        zoom: float | None = None,
        legend: bool = False,
        ports: bool = False,
    ):
        self.doc = doc
        self.width = width
        self.height = height
        self.ascii_mode = ascii_mode
        self.color = color
        self.theme = THEMES.get(theme, THEMES["default"])
        self.canvas = Canvas(width, height, ascii_mode, color)
        viewport = (doc.get("canvas") or {}).get("viewport") or {}
        self.view_x = float(view_x if view_x is not None else viewport.get("x", 0))
        self.view_y = float(view_y if view_y is not None else viewport.get("y", 0))
        self.zoom = float(zoom if zoom is not None else viewport.get("zoom", 1)) or 1
        self.legend = legend
        self.ports = ports
        self.nodes = {n["id"]: n for n in doc.get("nodes", [])}

    def render(self) -> str:
        self._draw_groups()
        self._draw_edges()
        self._draw_nodes()
        if self.ports:
            self._draw_ports()
        self._draw_annotations()
        if self.legend:
            self._draw_legend()
        return self.canvas.render()

    def _screen(self, p: Pt) -> tuple[int, int]:
        # Python round() uses banker rounding, which creates surprising one-cell
        # jogs at .5 endpoints. Terminal geometry wants stable half-up rounding.
        return (self._iround((p.x - self.view_x) * self.zoom), self._iround((p.y - self.view_y) * self.zoom))

    def _screen_rect(self, r: Rect) -> tuple[int, int, int, int]:
        x, y = self._screen(Pt(r.x, r.y))
        return x, y, max(2, round(r.w * self.zoom)), max(2, round(r.h * self.zoom))

    def _node_rect(self, node: dict[str, Any]) -> Rect:
        pos = node.get("position") or {"x": 0, "y": 0}
        size = node.get("size") or {"w": max(8, len(node.get("label", node["id"])) + 4), "h": 3}
        return Rect(float(pos["x"]), float(pos["y"]), float(size["w"]), float(size["h"]))

    def _draw_groups(self) -> None:
        for group in self.doc.get("groups", []):
            bounds = group.get("bounds")
            if not bounds:
                continue
            x, y, w, h = self._screen_rect(Rect(bounds["x"], bounds["y"], bounds["w"], bounds["h"]))
            style = group.get("style") or {}
            self.canvas.box(x, y, w, h, group.get("label", group["id"]), self._pen(style, "group"), "dotted")

    def _draw_nodes(self) -> None:
        for node in self.doc.get("nodes", []):
            rect = self._node_rect(node)
            x, y, w, h = self._screen_rect(rect)
            label = node.get("label", node["id"])
            style = node.get("style") or {}
            node_type = node.get("type", "node")
            pen = self._pen(style, node_type if node_type in self.theme else "node")
            shape = str(style.get("shape") or self._default_shape(node_type))
            border = str(style.get("border") or self._default_border(node_type))

            if shape == "diamond" or node_type == "decision":
                self.canvas.diamond(x + w // 2, y + h // 2, w, h, label, pen)
            elif shape == "cylinder" or node_type in {"database", "store"}:
                self.canvas.cylinder(x, y, w, h, label, pen)
            else:
                self.canvas.box(x, y, w, h, label, pen, border)

            body = node.get("body")
            if body and h >= 4 and shape not in {"diamond", "cylinder"}:
                body_pen = self._pen(style, "text")
                for i, line in enumerate(self._wrap_text(body, max(1, w - 4), max(0, h - 3))):
                    self.canvas.text(x + 2, y + 2 + i, line, body_pen, max_width=max(0, w - 4))

    def _draw_edges(self) -> None:
        labels: list[tuple[int, int, str, Pen]] = []
        for edge in self.doc.get("edges", []):
            src = self._endpoint(edge["source"], source=True)
            dst = self._endpoint(edge["target"], source=False)
            if src is None or dst is None:
                continue
            points = [src]
            for p in edge.get("points", []):
                points.append(Pt(float(p["x"]), float(p["y"])))
            points.append(dst)
            screen_pts = self._orthogonalize([self._screen(p) for p in points])
            style = edge.get("style") or {}
            edge_type = edge.get("type", "edge")
            pen = self._pen(style, edge_type if edge_type in self.theme else "edge")
            line = str(style.get("line", "solid"))
            self.canvas.polyline(screen_pts, pen, arrow_end=bool(style.get("arrow_end", True)), line=line)
            label = edge.get("label")
            if label and len(screen_pts) >= 2:
                label_text = f" {label} "
                lx, ly = self._edge_label_pos(screen_pts, label_text)
                labels.append((lx, ly, label_text, replace(pen, bold=True)))

        # Labels are drawn after all edge strokes so later edges do not slice
        # through label text. Nodes are still drawn afterward, so labels never
        # win over actual boxes.
        for lx, ly, label_text, pen in labels:
            self.canvas.text(lx, ly, label_text, pen, max_width=max(0, self.width - lx - 1))

    def _draw_ports(self) -> None:
        marker = "*" if self.ascii_mode else "●"
        for node in self.doc.get("nodes", []):
            ports = node.get("ports", [])
            if not ports:
                continue
            rect = self._node_rect(node)
            sx, sy, sw, sh = self._screen_rect(rect)
            style = node.get("style") or {}
            pen = self._pen(style, node.get("type", "node") if node.get("type", "node") in self.theme else "node")
            for port in ports:
                px, py = self._port_marker_point(sx, sy, sw, sh, str(port.get("side", "auto")))
                self.canvas.put(px, py, marker, pen, force=True)

    def _draw_annotations(self) -> None:
        for ann in self.doc.get("annotations", []):
            pos = ann.get("position") or {"x": 0, "y": 0}
            size = ann.get("size") or {"w": min(60, max(12, len(ann.get("body", "")) + 4)), "h": 3}
            x, y, w, h = self._screen_rect(Rect(pos["x"], pos["y"], size["w"], size["h"]))
            style = ann.get("style") or {}
            key = "warning" if ann.get("type") == "warning" else "annotation"
            pen = self._pen(style, key)
            self.canvas.box(x, y, w, h, ann.get("type", "note"), pen, str(style.get("border", "rounded")))
            for i, line in enumerate(self._wrap_text(ann.get("body", ""), max(1, w - 4), max(0, h - 2))):
                self.canvas.text(x + 2, y + 1 + i, line, self._pen(style, "text"), max_width=max(0, w - 4))

    def _draw_legend(self) -> None:
        y = max(0, self.height - 4)
        text = f"diagram-junky | {self.doc.get('id', 'unknown')} | zoom={self.zoom:g} view=({self.view_x:g},{self.view_y:g})"
        self.canvas.text(1, y, text, self.theme.get("muted", Pen()), max_width=self.width - 2)

    def _edge_label_pos(self, pts: list[tuple[int, int]], label: str) -> tuple[int, int]:
        # Pick the longest segment and place the label just off the line. This
        # avoids the first prototype's ugly habit of writing edge labels through
        # node body text when endpoints are close together.
        best = (pts[0], pts[1], -1)
        for a, b in zip(pts, pts[1:]):
            score = abs(b[0] - a[0]) + abs(b[1] - a[1])
            if score > best[2]:
                best = (a, b, score)
        a, b, _ = best
        if a[1] == b[1]:
            x = min(a[0], b[0]) + max(0, abs(b[0] - a[0]) - text_width(label)) // 2
            y = a[1] - 1 if a[1] > 0 else a[1] + 1
            return max(0, x), max(0, y)
        if a[0] == b[0]:
            return min(self.width - 1, a[0] + 1), max(0, min(a[1], b[1]) + abs(b[1] - a[1]) // 2)
        return max(0, (a[0] + b[0]) // 2 + 1), max(0, (a[1] + b[1]) // 2)

    def _endpoint(self, endpoint: dict[str, Any], source: bool) -> Pt | None:
        node = self.nodes.get(endpoint.get("node"))
        if not node:
            return None
        rect = self._node_rect(node)
        port_id = endpoint.get("port")
        port = None
        if port_id:
            port = next((p for p in node.get("ports", []) if p.get("id") == port_id), None)
        if port and "position" in port:
            p = port["position"]
            return Pt(rect.x + float(p["x"]), rect.y + float(p["y"]))
        side = (port or {}).get("side", "right" if source else "left")
        return self._side_point(rect, side, source)

    def _side_point(self, rect: Rect, side: str, source: bool) -> Pt:
        mid_x = rect.x + (rect.w - 1) / 2
        mid_y = rect.y + (rect.h - 1) / 2
        if side == "left":
            return Pt(rect.x - 1, mid_y)
        if side == "right":
            return Pt(rect.x + rect.w, mid_y)
        if side == "top":
            return Pt(mid_x, rect.y - 1)
        if side == "bottom":
            return Pt(mid_x, rect.y + rect.h)
        return Pt(rect.x + rect.w if source else rect.x - 1, mid_y)

    def _port_marker_point(self, x: int, y: int, w: int, h: int, side: str) -> tuple[int, int]:
        if side == "left":
            return x, y + h // 2
        if side == "right":
            return x + w - 1, y + h // 2
        if side == "top":
            return x + w // 2, y
        if side == "bottom":
            return x + w // 2, y + h - 1
        return x + w - 1, y + h // 2

    def _orthogonalize(self, pts: list[tuple[int, int]]) -> list[tuple[int, int]]:
        if len(pts) < 2:
            return pts
        if len(pts) == 2:
            a, b = pts
            if abs(a[1] - b[1]) <= 1 and abs(a[0] - b[0]) > 4:
                return [a, (b[0], a[1])]
            if abs(a[0] - b[0]) <= 1 and abs(a[1] - b[1]) > 4:
                return [a, (a[0], b[1])]
        out = [pts[0]]
        for nxt0 in pts[1:]:
            cur = out[-1]
            # Collapse tiny doglegs. A one-cell kink is technically accurate but
            # visually noisy; snap it clean unless the user supplied enough room.
            nxt = nxt0
            if abs(cur[0] - nxt[0]) <= 1 and abs(cur[1] - nxt[1]) > 2:
                nxt = (cur[0], nxt[1])
            elif abs(cur[1] - nxt[1]) <= 1 and abs(cur[0] - nxt[0]) > 2:
                nxt = (nxt[0], cur[1])
            if cur[0] == nxt[0] or cur[1] == nxt[1]:
                out.append(nxt)
            else:
                out.append((nxt[0], cur[1]))
                out.append(nxt)
        return self._dedupe(out)

    def _dedupe(self, pts: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
        out: list[tuple[int, int]] = []
        for p in pts:
            if not out or out[-1] != p:
                out.append(p)
        return out

    def _wrap_text(self, text: str, width: int, max_lines: int) -> list[str]:
        if width <= 0 or max_lines <= 0:
            return []
        lines: list[str] = []
        for raw in text.splitlines():
            words = raw.split()
            if not words:
                lines.append("")
                continue
            current = words[0]
            for word in words[1:]:
                if text_width(current) + 1 + text_width(word) <= width:
                    current += " " + word
                else:
                    lines.append(current)
                    current = word
                    if len(lines) >= max_lines:
                        break
            if len(lines) < max_lines:
                lines.append(current)
            if len(lines) >= max_lines:
                break
        if len(lines) > max_lines:
            lines = lines[:max_lines]
        return [fit_text(line, width) for line in lines]

    def _iround(self, value: float) -> int:
        return math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5)

    def _pen(self, style: dict[str, Any], fallback: str) -> Pen:
        base = self.theme.get(fallback, self.theme.get("text", Pen()))
        fg = style.get("stroke") or style.get("text_color") or base.fg
        bg = style.get("fill") or base.bg
        if fg == "none":
            fg = None
        if bg == "none":
            bg = None
        return Pen(
            fg=fg,
            bg=bg,
            bold=bool(style.get("bold", base.bold)),
            dim=bool(style.get("dim", base.dim)),
            italic=bool(style.get("italic", base.italic)),
            inverse=bool(style.get("selected", base.inverse)),
        )

    def _default_shape(self, node_type: str) -> str:
        if node_type == "decision":
            return "diamond"
        if node_type in {"database", "store"}:
            return "cylinder"
        return "box"

    def _default_border(self, node_type: str) -> str:
        return {
            "terminator": "rounded",
            "external": "double",
            "collection": "heavy",
            "actor": "rounded",
            "note": "rounded",
        }.get(node_type, "square")


def color_enabled(mode: str) -> bool:
    if mode == "always":
        return True
    if mode == "never":
        return False
    if os.environ.get("NO_COLOR"):
        return False
    return sys.stdout.isatty()


def diagram_bounds(doc: dict[str, Any]) -> Rect:
    xs: list[float] = []
    ys: list[float] = []

    def add_rect(x: float, y: float, w: float, h: float) -> None:
        xs.extend([x, x + max(1, w)])
        ys.extend([y, y + max(1, h)])

    def add_pt(x: float, y: float) -> None:
        xs.append(x)
        ys.append(y)

    for node in doc.get("nodes", []):
        pos = node.get("position") or {"x": 0, "y": 0}
        size = node.get("size") or {"w": max(8, len(node.get("label", node.get("id", ""))) + 4), "h": 3}
        add_rect(float(pos.get("x", 0)), float(pos.get("y", 0)), float(size.get("w", 8)), float(size.get("h", 3)))
    for group in doc.get("groups", []):
        bounds = group.get("bounds")
        if bounds:
            add_rect(float(bounds["x"]), float(bounds["y"]), float(bounds["w"]), float(bounds["h"]))
    for ann in doc.get("annotations", []):
        pos = ann.get("position") or {"x": 0, "y": 0}
        size = ann.get("size") or {"w": 20, "h": 3}
        add_rect(float(pos.get("x", 0)), float(pos.get("y", 0)), float(size.get("w", 20)), float(size.get("h", 3)))
    for edge in doc.get("edges", []):
        for p in edge.get("points", []):
            add_pt(float(p.get("x", 0)), float(p.get("y", 0)))

    if not xs or not ys:
        return Rect(0, 0, 1, 1)
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    return Rect(min_x, min_y, max(1, max_x - min_x), max(1, max_y - min_y))


def fit_viewport(
    doc: dict[str, Any], width: int, height: int, margin: int, scale: bool, upscale: bool
) -> tuple[float, float, float]:
    b = diagram_bounds(doc)
    zoom = 1.0
    if scale:
        usable_w = max(1, width - margin * 2)
        usable_h = max(1, height - margin * 2)
        zoom = min(usable_w / max(1, b.w), usable_h / max(1, b.h))
        if not upscale:
            zoom = min(1.0, zoom)
        zoom = max(0.05, zoom)
    return b.x - margin / zoom, b.y - margin / zoom, zoom


def list_examples() -> str:
    lines = ["examples:"]
    for path in sorted(EXAMPLES_DIR.glob("*.diagram.json")):
        try:
            doc = json.loads(path.read_text())
            title = doc.get("title", path.stem)
            kind = doc.get("kind", "unknown")
            lines.append(f"  {path.name.removesuffix('.diagram.json'):<18} {kind:<12} {title}")
        except Exception:
            lines.append(f"  {path.name.removesuffix('.diagram.json'):<18} invalid")
    return "\n".join(lines) + "\n"


def list_styles() -> str:
    return (
        "themes: " + ", ".join(sorted(THEMES)) + "\n"
        "borders: " + ", ".join(k for k in BORDERS if k != "ascii") + "\n"
        "lines: solid, dashed, dotted, heavy, double\n"
        "node shapes: box, diamond, cylinder\n"
        "presets: " + ", ".join(sorted(PRESETS)) + "\n"
    )


def resolve_diagram(diagram: Path | None, example: str | None) -> Path:
    if example:
        candidate = EXAMPLES_DIR / example
        if candidate.suffix != ".json":
            candidate = EXAMPLES_DIR / f"{example}.diagram.json"
        if not candidate.exists():
            raise FileNotFoundError(f"unknown example: {example}\n{list_examples().rstrip()}")
        return candidate
    if diagram is None:
        raise FileNotFoundError("missing diagram path. Use --examples or --example <name>.")
    return diagram


def load_doc(path: Path, *, safe: bool = False) -> dict[str, Any]:
    """Load a diagram document. If safe=True, never raises — returns a placeholder.

    The TUI calls this on untrusted user files and generated agent examples.
    CLI mode keeps safe=False so schema violations are loud failures.
    """
    try:
        raw = path.read_text()
    except OSError as e:
        if safe:
            return {"schema_version": "diagram.document.v0", "id": path.stem, "title": path.stem, "kind": "broken"}
        raise
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as e:
        if safe:
            return {"schema_version": "diagram.document.v0", "id": path.stem, "title": path.stem, "kind": "invalid-json"}
        raise ValueError(f"invalid JSON in {path}: {e}") from e
    if doc.get("schema_version") != "diagram.document.v0":
        if safe:
            doc["schema_version"] = "diagram.document.v0"
            return doc
        raise ValueError(f"unsupported schema_version {doc.get('schema_version')!r}")
    return doc


def validate_doc(doc: dict[str, Any]) -> tuple[bool, str]:
    try:
        import jsonschema  # type: ignore
    except Exception as e:
        return False, f"jsonschema unavailable: {e}"
    schema = json.loads(DOC_SCHEMA.read_text())
    jsonschema.Draft202012Validator(schema).validate(doc)
    return True, "schema ok"


def inspect_doc(doc: dict[str, Any], path: Path) -> str:
    b = diagram_bounds(doc)
    ports = sum(len(n.get("ports", [])) for n in doc.get("nodes", []))
    node_types: dict[str, int] = {}
    edge_types: dict[str, int] = {}
    for n in doc.get("nodes", []):
        node_types[n.get("type", "node")] = node_types.get(n.get("type", "node"), 0) + 1
    for e in doc.get("edges", []):
        edge_types[e.get("type", "edge")] = edge_types.get(e.get("type", "edge"), 0) + 1
    return "\n".join(
        [
            f"file: {path}",
            f"id: {doc.get('id')}",
            f"title: {doc.get('title', '')}",
            f"kind: {doc.get('kind')}",
            f"nodes: {len(doc.get('nodes', []))}  edges: {len(doc.get('edges', []))}  ports: {ports}",
            f"groups: {len(doc.get('groups', []))}  annotations: {len(doc.get('annotations', []))}",
            f"bounds: x={b.x:g} y={b.y:g} w={b.w:g} h={b.h:g}",
            "node_types: " + (", ".join(f"{k}={v}" for k, v in sorted(node_types.items())) or "none"),
            "edge_types: " + (", ".join(f"{k}={v}" for k, v in sorted(edge_types.items())) or "none"),
        ]
    ) + "\n"


def apply_preset(ns: argparse.Namespace, term: os.terminal_size) -> None:
    if not ns.preset:
        return
    preset = PRESETS[ns.preset]
    if ns.width is None and "width" in preset:
        ns.width = min(int(preset["width"]), max(int(preset["width"]), term.columns))
    if ns.height is None and "height" in preset:
        ns.height = min(int(preset["height"]), max(int(preset["height"]), term.lines - 2))
    if ns.theme is None and "theme" in preset:
        ns.theme = preset["theme"]
    for flag in ("fit", "legend", "ports"):
        if getattr(ns, flag) is False and preset.get(flag):
            setattr(ns, flag, True)


def render_once(doc: dict[str, Any], ns: argparse.Namespace) -> str:
    view_x, view_y, zoom = ns.x, ns.y, ns.zoom
    if ns.fit:
        fit_height = ns.height - (4 if ns.legend else 0)
        fx, fy, fz = fit_viewport(doc, ns.width, fit_height, ns.margin, ns.fit_scale, ns.fit_upscale)
        view_x = fx if view_x is None else view_x
        view_y = fy if view_y is None else view_y
        zoom = fz if zoom is None else zoom
    return Renderer(
        doc,
        ns.width,
        ns.height,
        ascii_mode=ns.ascii,
        color=color_enabled(ns.color),
        theme=ns.theme,
        view_x=view_x,
        view_y=view_y,
        zoom=zoom,
        legend=ns.legend,
        ports=ns.ports,
    ).render()


def main(argv: list[str]) -> int:
    term = shutil.get_terminal_size((120, 40))
    ap = argparse.ArgumentParser(description="Render diagram.document.v0 JSON to a terminal canvas")
    ap.add_argument("diagram", type=Path, nargs="?", help="Path to *.diagram.json")
    ap.add_argument("--example", help="Render example by name from playground/diagram-junky/examples")
    ap.add_argument("--examples", action="store_true", help="List bundled examples and exit")
    ap.add_argument("--styles", action="store_true", help="List themes, border styles, line styles, presets and exit")
    ap.add_argument("--preset", choices=sorted(PRESETS), help="Apply a renderer preset")
    ap.add_argument("--width", type=int, default=None, help="Canvas width in characters")
    ap.add_argument("--height", type=int, default=None, help="Canvas height in characters")
    ap.add_argument("--ascii", action="store_true", help="Use ASCII instead of Unicode box drawing")
    ap.add_argument("--color", choices=["auto", "always", "never"], default="auto", help="ANSI color policy")
    ap.add_argument("--theme", choices=sorted(THEMES), default=None, help="Color theme")
    ap.add_argument("--x", type=float, default=None, help="Viewport x override")
    ap.add_argument("--y", type=float, default=None, help="Viewport y override")
    ap.add_argument("--zoom", type=float, default=None, help="Viewport zoom override")
    ap.add_argument("--fit", action="store_true", help="Pan viewport to diagram bounds")
    ap.add_argument("--fit-scale", action="store_true", help="Allow --fit to downscale when needed")
    ap.add_argument("--fit-upscale", action="store_true", help="Allow --fit-scale to zoom above 1x")
    ap.add_argument("--margin", type=int, default=2, help="Canvas margin used by --fit")
    ap.add_argument("--legend", action="store_true", help="Draw renderer/debug legend")
    ap.add_argument("--ports", action="store_true", help="Draw port markers on node borders")
    ap.add_argument("--inspect", action="store_true", help="Print document summary before rendering")
    ap.add_argument("--validate", action="store_true", help="Validate document against diagram-document schema")
    ap.add_argument("--bounds", action="store_true", help="Print computed logical bounds and exit")
    ap.add_argument("--output", type=Path, help="Write rendered output to file instead of stdout")
    ap.add_argument("--watch", type=float, nargs="?", const=1.0, help="Re-render every N seconds")
    ap.add_argument("--no-clear", action="store_true", help="Do not clear the terminal in --watch mode")
    ns = ap.parse_args(argv)

    if ns.examples:
        sys.stdout.write(list_examples())
        return 0
    if ns.styles:
        sys.stdout.write(list_styles())
        return 0

    apply_preset(ns, term)
    if ns.width is None:
        ns.width = term.columns
    if ns.height is None:
        ns.height = min(60, max(20, term.lines - 2))
    if ns.theme is None:
        ns.theme = "default"

    try:
        path = resolve_diagram(ns.diagram, ns.example)
        doc = load_doc(path)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    if ns.validate:
        try:
            ok, msg = validate_doc(doc)
        except Exception as e:
            print(f"schema error: {e}", file=sys.stderr)
            return 2
        print(msg, file=sys.stderr if not ok else sys.stdout)
        if not ok:
            return 2

    if ns.inspect:
        sys.stdout.write(inspect_doc(doc, path))

    if ns.bounds:
        b = diagram_bounds(doc)
        sys.stdout.write(f"x={b.x:g} y={b.y:g} w={b.w:g} h={b.h:g}\n")
        return 0

    def emit() -> None:
        fresh = load_doc(path)
        rendered = render_once(fresh, ns)
        if ns.output:
            ns.output.parent.mkdir(parents=True, exist_ok=True)
            ns.output.write_text(rendered)
        else:
            sys.stdout.write(rendered)
            sys.stdout.flush()

    if ns.watch:
        while True:
            if not ns.no_clear and not ns.output:
                sys.stdout.write("\033[2J\033[H")
            emit()
            if not ns.output:
                sys.stdout.write(f"\nwatching {path} every {ns.watch:g}s — Ctrl-C to stop\n")
                sys.stdout.flush()
            time.sleep(max(0.1, ns.watch))

    emit()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
