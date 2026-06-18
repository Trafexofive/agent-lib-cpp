#!/usr/bin/env python3
"""diagram-junky CLI renderer.

Prototype renderer for `diagram.document.v0` files. The important split is:

    Diagram JSON -> logical scene -> character canvas

The TUI can keep the same logical scene and swap the final canvas target later.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


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


UNICODE = {
    "h": "─",
    "v": "│",
    "tl": "┌",
    "tr": "┐",
    "bl": "└",
    "br": "┘",
    "cross": "┼",
    "tee_r": "├",
    "tee_l": "┤",
    "tee_d": "┬",
    "tee_u": "┴",
    "arrow_r": "▶",
    "arrow_l": "◀",
    "arrow_u": "▲",
    "arrow_d": "▼",
    "dot": "•",
}

ASCII = {
    "h": "-",
    "v": "|",
    "tl": "+",
    "tr": "+",
    "bl": "+",
    "br": "+",
    "cross": "+",
    "tee_r": "+",
    "tee_l": "+",
    "tee_d": "+",
    "tee_u": "+",
    "arrow_r": ">",
    "arrow_l": "<",
    "arrow_u": "^",
    "arrow_d": "v",
    "dot": "*",
}


class Canvas:
    def __init__(self, width: int, height: int, chars: dict[str, str]):
        self.width = max(1, width)
        self.height = max(1, height)
        self.chars = chars
        self.grid = [[" " for _ in range(self.width)] for _ in range(self.height)]

    def put(self, x: int, y: int, ch: str) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            self.grid[y][x] = self._merge(self.grid[y][x], ch)

    def text(self, x: int, y: int, s: str, max_width: int | None = None) -> None:
        if y < 0 or y >= self.height:
            return
        if max_width is not None:
            s = s[: max(0, max_width)]
        for i, ch in enumerate(s):
            if 0 <= x + i < self.width:
                self.grid[y][x + i] = ch

    def hline(self, x1: int, x2: int, y: int) -> None:
        if y < 0 or y >= self.height:
            return
        a, b = sorted((x1, x2))
        for x in range(a, b + 1):
            self.put(x, y, self.chars["h"])

    def vline(self, x: int, y1: int, y2: int) -> None:
        if x < 0 or x >= self.width:
            return
        a, b = sorted((y1, y2))
        for y in range(a, b + 1):
            self.put(x, y, self.chars["v"])

    def box(self, x: int, y: int, w: int, h: int, label: str = "") -> None:
        if w < 2 or h < 2:
            return
        self.hline(x + 1, x + w - 2, y)
        self.hline(x + 1, x + w - 2, y + h - 1)
        self.vline(x, y + 1, y + h - 2)
        self.vline(x + w - 1, y + 1, y + h - 2)
        self.put(x, y, self.chars["tl"])
        self.put(x + w - 1, y, self.chars["tr"])
        self.put(x, y + h - 1, self.chars["bl"])
        self.put(x + w - 1, y + h - 1, self.chars["br"])
        if label:
            self.text(x + 2, y, f" {label} ", max_width=max(0, w - 4))

    def polyline(self, pts: list[tuple[int, int]], arrow_end: bool = False) -> None:
        if len(pts) < 2:
            return
        for a, b in zip(pts, pts[1:]):
            self._segment(a, b)
        if arrow_end:
            self._arrow(pts[-2], pts[-1])

    def render(self) -> str:
        return "\n".join("".join(row).rstrip() for row in self.grid).rstrip() + "\n"

    def _segment(self, a: tuple[int, int], b: tuple[int, int]) -> None:
        x1, y1 = a
        x2, y2 = b
        if x1 == x2:
            self.vline(x1, y1, y2)
            return
        if y1 == y2:
            self.hline(x1, x2, y1)
            return

        # Fallback for diagonal/freeform segments. Bresenham-ish, intentionally
        # simple: this is still the CLI target, not the model contract.
        steps = max(abs(x2 - x1), abs(y2 - y1))
        for i in range(steps + 1):
            t = i / max(1, steps)
            x = round(x1 + (x2 - x1) * t)
            y = round(y1 + (y2 - y1) * t)
            self.put(x, y, self.chars["dot"])

    def _arrow(self, a: tuple[int, int], b: tuple[int, int]) -> None:
        dx = b[0] - a[0]
        dy = b[1] - a[1]
        if abs(dx) >= abs(dy):
            self.put(b[0], b[1], self.chars["arrow_r"] if dx >= 0 else self.chars["arrow_l"])
        else:
            self.put(b[0], b[1], self.chars["arrow_d"] if dy >= 0 else self.chars["arrow_u"])

    def _merge(self, old: str, new: str) -> str:
        if old == " " or old == new:
            return new
        line_chars = {self.chars["h"], self.chars["v"], self.chars["dot"]}
        if old in line_chars and new in line_chars and old != new:
            return self.chars["cross"]
        # Keep node borders/text over edges when drawn later.
        if new not in line_chars:
            return new
        return old


class Renderer:
    def __init__(self, doc: dict[str, Any], width: int, height: int, ascii_mode: bool = False):
        self.doc = doc
        self.width = width
        self.height = height
        self.chars = ASCII if ascii_mode else UNICODE
        self.canvas = Canvas(width, height, self.chars)
        viewport = (doc.get("canvas") or {}).get("viewport") or {}
        self.view_x = float(viewport.get("x", 0))
        self.view_y = float(viewport.get("y", 0))
        self.zoom = float(viewport.get("zoom", 1)) or 1
        self.nodes = {n["id"]: n for n in doc.get("nodes", [])}

    def render(self) -> str:
        self._draw_groups()
        self._draw_edges()
        self._draw_nodes()
        self._draw_annotations()
        return self.canvas.render()

    def _screen(self, p: Pt) -> tuple[int, int]:
        return (round((p.x - self.view_x) * self.zoom), round((p.y - self.view_y) * self.zoom))

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
            self.canvas.box(x, y, w, h, group.get("label", group["id"]))

    def _draw_nodes(self) -> None:
        for node in self.doc.get("nodes", []):
            rect = self._node_rect(node)
            x, y, w, h = self._screen_rect(rect)
            label = node.get("label", node["id"])
            self.canvas.box(x, y, w, h, label)
            body = node.get("body")
            if body and h >= 4:
                for i, line in enumerate(body.splitlines()[: max(0, h - 3)]):
                    self.canvas.text(x + 2, y + 2 + i, line, max_width=max(0, w - 4))

    def _draw_edges(self) -> None:
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
            self.canvas.polyline(screen_pts, arrow_end=bool(style.get("arrow_end", True)))
            label = edge.get("label")
            if label and len(screen_pts) >= 2:
                mid = screen_pts[len(screen_pts) // 2]
                self.canvas.text(mid[0] + 1, mid[1], label)

    def _draw_annotations(self) -> None:
        for ann in self.doc.get("annotations", []):
            pos = ann.get("position") or {"x": 0, "y": 0}
            size = ann.get("size") or {"w": min(60, max(12, len(ann.get("body", "")) + 4)), "h": 3}
            x, y, w, h = self._screen_rect(Rect(pos["x"], pos["y"], size["w"], size["h"]))
            self.canvas.box(x, y, w, h, ann.get("type", "note"))
            for i, line in enumerate(ann.get("body", "").splitlines()[: max(0, h - 2)]):
                self.canvas.text(x + 2, y + 1 + i, line, max_width=max(0, w - 4))

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
        # Return the first canvas point outside the node border. Nodes are drawn
        # after edges, so interior/border edge pixels would be hidden anyway.
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

    def _orthogonalize(self, pts: list[tuple[int, int]]) -> list[tuple[int, int]]:
        if len(pts) < 2:
            return pts
        out = [pts[0]]
        for nxt in pts[1:]:
            cur = out[-1]
            if cur[0] == nxt[0] or cur[1] == nxt[1]:
                out.append(nxt)
            else:
                # horizontal then vertical keeps flow diagrams readable.
                out.append((nxt[0], cur[1]))
                out.append(nxt)
        return self._dedupe(out)

    def _dedupe(self, pts: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
        out: list[tuple[int, int]] = []
        for p in pts:
            if not out or out[-1] != p:
                out.append(p)
        return out


def main(argv: list[str]) -> int:
    term = shutil.get_terminal_size((120, 40))
    ap = argparse.ArgumentParser(description="Render diagram.document.v0 JSON to a terminal canvas")
    ap.add_argument("diagram", type=Path, help="Path to *.diagram.json")
    ap.add_argument("--width", type=int, default=term.columns, help="Canvas width in characters")
    ap.add_argument("--height", type=int, default=min(60, max(20, term.lines - 2)), help="Canvas height in characters")
    ap.add_argument("--ascii", action="store_true", help="Use ASCII instead of Unicode box drawing")
    ns = ap.parse_args(argv)

    doc = json.loads(ns.diagram.read_text())
    if doc.get("schema_version") != "diagram.document.v0":
        print(f"error: unsupported schema_version {doc.get('schema_version')!r}", file=sys.stderr)
        return 2
    sys.stdout.write(Renderer(doc, ns.width, ns.height, ns.ascii).render())
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
