#!/usr/bin/env python3
"""Raw ANSI diagram-junky TUI prototype.

No curses dependency: this is deliberately small and playground-local. It uses
``diagram_junky.rendering.Renderer`` directly so the future C++/canvas hub can
keep the same scene/canvas split while we iterate on interaction feel here.
"""

from __future__ import annotations

import argparse
import json
import select
import shutil
import sys
import termios
import time
import tty
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from diagram_junky.rendering import EXAMPLES_DIR, THEMES, Renderer, fit_viewport, load_doc

ROOT = Path(__file__).resolve().parent
DEFAULT_STATE = Path.home() / ".cache" / "diagram-junky" / "tui-state.json"
THEME_ORDER = ["default", "mono", "neon"]


@dataclass
class View:
    x: float = 0.0
    y: float = 0.0
    zoom: float = 1.0


class RawMode:
    def __enter__(self) -> "RawMode":
        self.fd = sys.stdin.fileno()
        self.old = termios.tcgetattr(self.fd)
        tty.setcbreak(self.fd)
        return self

    def __exit__(self, *_: object) -> None:
        termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old)


class DiagramTui:
    def __init__(self, path: Path | None, example: str | None, state_path: Path, color: bool):
        self.state_path = state_path
        self.color = color
        self.examples = sorted(EXAMPLES_DIR.glob("*.diagram.json"))
        self.example_index = 0
        self.path = self._resolve_path(path, example)
        self.doc = load_doc(self.path)
        self.view = View()
        self.theme = "neon"
        self.legend = True
        self.ports = False
        self.message = "h/j/k/l or arrows pan · +/- zoom · n/p examples · f fit · t theme · s save · q quit"
        self._load_state_if_relevant(path, example)

    def _resolve_path(self, path: Path | None, example: str | None) -> Path:
        if example:
            p = EXAMPLES_DIR / (example if example.endswith(".json") else f"{example}.diagram.json")
            if not p.exists():
                raise FileNotFoundError(f"unknown example: {example}")
            return p
        if path:
            return path
        return self.examples[0] if self.examples else ROOT / "examples" / "minimal-flow.diagram.json"

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
            self.message = f"loaded state from {self.state_path}"
        except Exception as e:
            self.message = f"state load skipped: {e}"

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
        canvas_h = max(5, height - 3)
        rendered = Renderer(
            self.doc,
            width,
            canvas_h,
            ascii_mode=False,
            color=self.color,
            theme=self.theme,
            view_x=self.view.x,
            view_y=self.view.y,
            zoom=self.view.zoom,
            legend=self.legend,
            ports=self.ports,
        ).render().rstrip("\n")
        status = self.status_line(width)
        hint = self.message[: max(0, width - 1)]
        return f"{rendered}\n\033[7m{status:<{width}}\033[0m\n{hint}\n"

    def draw(self) -> None:
        size = shutil.get_terminal_size((120, 36))
        sys.stdout.write("\033[H\033[2J")
        sys.stdout.write(self.frame(size.columns, size.lines))
        sys.stdout.flush()

    def status_line(self, width: int) -> str:
        title = self.doc.get("title", self.doc.get("id", self.path.name))
        rel = self.path.relative_to(ROOT) if self.path.is_relative_to(ROOT) else self.path
        return (
            f" diagram-junky TUI | {title} | {rel} | theme={self.theme} "
            f"zoom={self.view.zoom:.2f} view=({self.view.x:.1f},{self.view.y:.1f}) "
            f"ports={'on' if self.ports else 'off'} legend={'on' if self.legend else 'off'} "
        )[:width]

    def read_key(self) -> str:
        while True:
            r, _, _ = select.select([sys.stdin], [], [], 0.25)
            if r:
                ch = sys.stdin.read(1)
                if ch == "\x1b":
                    # Decode common arrow key escape sequences.
                    r2, _, _ = select.select([sys.stdin], [], [], 0.01)
                    if r2:
                        seq = sys.stdin.read(2)
                        return {"[A": "up", "[B": "down", "[C": "right", "[D": "left"}.get(seq, ch + seq)
                return ch

    def handle_key(self, key: str) -> bool:
        pan = max(1.0, 4.0 / max(0.25, self.view.zoom))
        if key in {"q", "\x03"}:
            return False
        if key in {"h", "left"}:
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
        elif key == "t":
            i = (THEME_ORDER.index(self.theme) + 1) % len(THEME_ORDER)
            self.theme = THEME_ORDER[i]
        elif key == "c":
            self.color = not self.color
        elif key == "g":
            self.legend = not self.legend
        elif key == "o":
            self.ports = not self.ports
        elif key == "n":
            self.open_example(1)
        elif key == "p":
            self.open_example(-1)
        elif key == "r":
            self.doc = load_doc(self.path)
            self.message = f"reloaded {self.path.name}"
        elif key == "s":
            self.save_state()
        elif key == "?":
            self.message = "keys: q quit · arrows/hjkl pan · +/- zoom · 0 reset · f fit · n/p examples · t/c/g/o toggles · r reload · s save"
        return True

    def fit(self) -> None:
        size = shutil.get_terminal_size((120, 36))
        x, y, z = fit_viewport(self.doc, size.columns, max(5, size.lines - 3), margin=2, scale=True, upscale=False)
        self.view = View(x, y, z)
        self.message = "fit viewport to diagram bounds"

    def open_example(self, delta: int) -> None:
        if not self.examples:
            self.message = "no examples found"
            return
        try:
            self.example_index = self.examples.index(self.path)
        except ValueError:
            self.example_index = 0
        self.example_index = (self.example_index + delta) % len(self.examples)
        self.path = self.examples[self.example_index]
        self.doc = load_doc(self.path)
        self.view = View()
        self.message = f"opened {self.path.name}"

    def save_state(self) -> None:
        payload: dict[str, Any] = {
            "path": str(self.path),
            "x": self.view.x,
            "y": self.view.y,
            "zoom": self.view.zoom,
            "theme": self.theme,
            "legend": self.legend,
            "ports": self.ports,
            "saved_at": time.time(),
        }
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(payload, indent=2))
        self.message = f"saved state to {self.state_path}"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Raw ANSI diagram-junky TUI prototype")
    ap.add_argument("diagram", type=Path, nargs="?", help="Path to *.diagram.json")
    ap.add_argument("--example", help="Bundled example name")
    ap.add_argument("--state", type=Path, default=DEFAULT_STATE, help="Viewport/session state file")
    ap.add_argument("--no-color", action="store_true", help="Disable ANSI colors in rendered canvas")
    ap.add_argument("--smoke-render", action="store_true", help="Render one frame and exit for tests/CI")
    args = ap.parse_args(argv)

    app = DiagramTui(args.diagram, args.example, args.state, color=not args.no_color)
    if args.smoke_render:
        size = shutil.get_terminal_size((100, 28))
        sys.stdout.write(app.frame(size.columns, size.lines))
        return 0
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
