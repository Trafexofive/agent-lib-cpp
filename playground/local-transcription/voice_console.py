#!/usr/bin/env python3
"""voice_console.py — Textual TUI for the voice pipeline.

Shows the live pipeline (WAITING → LISTENING → THINKING → SPEAKING), the mic
level, and the conversation (you / agent reply). Type a prompt to run a manual
harness turn. Polls voice.py's --state-out JSON every 200ms.

Run with the backend:  ../examples/voice-console/run.sh
Keys: q / Ctrl-C to quit.
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
from pathlib import Path

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.reactive import reactive
from textual.widgets import Footer, Header, Input, RichLog, Static

STATE = Path(os.environ.get("VOICE_STATE", "/tmp/voice_console_state.json"))
REPO = Path(__file__).resolve().parents[2]
BIN = os.environ.get("CORTEX_BIN", str(REPO / "cortex-mk3"))
MANIFEST = os.environ.get(
    "VOICE_MANIFEST",
    str(REPO / "playground/local-transcription/manifests/agents/voice/agent.yml"),
)

# stage → (human label, color)
STAGES = {
    "starting": ("STARTING…", "yellow"),
    "wake": ("LISTENING FOR WAKE — say \"morpheus\"", "dim"),
    "idle": ("LISTENING FOR WAKE — say \"morpheus\"", "dim"),
    "waked": ("✔ WAKE DETECTED — listening…", "green"),
    "listen": ("LISTENING FOR COMMAND…", "cyan"),
    "stt": ("TRANSCRIBING…", "magenta"),
    "llm": ("THINKING…", "yellow"),
    "tts": ("SPEAKING…", "green"),
}


# ── status banner (current stage, big & clear) ─────────────────────────────
class Status(Static):
    def render(self) -> str:
        st = self.app.state.get("stage", "")
        label, color = STAGES.get(st, ("IDLE", "dim"))
        status = self.app.state.get("status", "")
        if st == "starting" and status:
            label = status.upper()
        hist = "  →  ".join(self.app._history) if self.app._history else ""
        return f"[bold {color}]▎{label}[/]\n[dim]chain: {hist}[/]"


# ── mic level bar ──────────────────────────────────────────────────────────
class Level(Static):
    def render(self) -> str:
        lvl = self.app.state.get("level", 0.0) or 0.0
        w = 40
        filled = min(w, int(lvl * w))
        color = "green" if lvl < 0.6 else ("yellow" if lvl < 0.85 else "red")
        bar = "█" * filled + "░" * (w - filled)
        return f"[bold {color}]{bar}[/] {int(lvl*100)}%"


# ── conversation (you / agent reply) ───────────────────────────────────────
class Conversation(Static):
    def render(self) -> str:
        t = (self.app.state.get("transcript", "") or "").strip()
        r = (self.app.state.get("response", "") or "").strip()
        if not t and not r:
            return "[dim]— no conversation yet —[/]"
        out = ""
        if t:
            out += f"[bold cyan]you:[/] {t}\n"
        if r:
            out += f"[bold green]agent:[/] {r}"
        return out


class VoiceConsole(App):
    CSS = """
    Screen { layout: vertical; }
    #status { height: 3; padding: 0 1; }
    #level { height: 3; padding: 0 1; }
    #conv { height: 8; border: round $accent; padding: 1; overflow-y: auto; }
    #blocks { height: 1fr; border: round $surface; }
    #prompt { dock: bottom; }
    """
    state: reactive[dict] = reactive({}, always_update=True)
    BINDINGS = [("q", "quit", "Quit"), ("ctrl+c", "quit", "Quit")]

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield Status(id="status")
        yield Level(id="level")
        yield Conversation(id="conv")
        yield RichLog(id="blocks", markup=True, wrap=True, highlight=True, max_lines=500)
        yield Input(placeholder="ask the harness… (Enter run · Esc clear · q quit)", id="prompt")
        yield Footer()

    def on_mount(self) -> None:
        self.set_interval(0.2, self._poll)
        self.query_one("#blocks", RichLog).write("[dim]— harness blocks —[/]")
        self._own_backend = str(REPO / "playground/local-transcription/voice.py")
        self._history: list[str] = []
        self._last_conv: tuple = ("", "")

    def on_unmount(self) -> None:
        import subprocess as _sp

        try:
            _sp.run(["pkill", "-f", self._own_backend], capture_output=True)
        except Exception:
            pass

    def _poll(self) -> None:
        try:
            self.state = json.loads(STATE.read_text())
        except Exception:
            self.state = {}
        st = self.state.get("stage", "")
        if st and (not self._history or self._history[-1] != st):
            self._history.append(st)
            self._history = self._history[-8:]  # keep last 8
        # cheap widgets refresh every poll
        for wid in ("#status", "#level"):
            try:
                self.query_one(wid).refresh()
            except Exception:
                pass
        # conversation only re-renders when its content changed
        cur = (self.state.get("transcript", ""), self.state.get("response", ""))
        if cur != self._last_conv:
            self._last_conv = cur
            try:
                self.query_one("#conv").refresh()
            except Exception:
                pass

    def on_input_submitted(self, event: Input.Submitted) -> None:
        prompt = event.value.strip()
        if not prompt:
            return
        event.input.value = ""
        blocks = self.query_one("#blocks", RichLog)
        blocks.write(f"[bold cyan]> {prompt}[/]")
        threading.Thread(target=self._run_harness, args=(prompt,), daemon=True).start()

    def _run_harness(self, prompt: str) -> None:
        """Stream a harness turn's blocks to the log as they arrive (no block)."""
        blocks = self.query_one("#blocks", RichLog)
        try:
            proc = subprocess.Popen(
                [BIN, "-m", MANIFEST, "run", "-p", prompt, "--ephemeral", "--no-ansi"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
            )
        except Exception as e:
            self.call_from_thread(blocks.write, f"[red]error: {e}[/]")
            return
        assert proc.stdout is not None
        for line in proc.stdout:
            s = line.strip()
            if not s:
                continue
            if s.startswith("[response]"):
                self.call_from_thread(blocks.write, f"[green]{line.rstrip()}[/]")
            elif s.startswith("[thought]"):
                self.call_from_thread(blocks.write, f"[dim]{line.rstrip()}[/]")
            elif s.startswith("[action") or s.startswith("[result"):
                self.call_from_thread(blocks.write, f"[yellow]{line.rstrip()}[/]")
            else:
                self.call_from_thread(blocks.write, line.rstrip())
        try:
            proc.wait(timeout=120)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    VoiceConsole().run()
