#!/usr/bin/env python3
"""MK3 tool wrapper around playground/diagram-junky/render.py."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[4]
RENDER = PROJECT_ROOT / "render.py"

BOOL_FLAGS = {
    "ascii": "--ascii",
    "fit": "--fit",
    "fit_scale": "--fit-scale",
    "fit_upscale": "--fit-upscale",
    "legend": "--legend",
    "ports": "--ports",
}
VALUE_FLAGS = {
    "preset": "--preset",
    "width": "--width",
    "height": "--height",
    "theme": "--theme",
    "color": "--color",
    "margin": "--margin",
    "output": "--output",
}
ACTIONS = {"render", "inspect", "validate", "bounds", "examples", "styles"}


def read_params() -> dict[str, Any]:
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        return {}
    arg = sys.argv[1]
    # cortex-mk3 executeScriptTool writes params to a temp file and passes the
    # path. The Tool class's own executeScript passes JSON directly. Handle both.
    if arg.startswith("/") or arg.startswith("./") or arg.startswith("../"):
        try:
            return json.loads(Path(arg).read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            pass
    return json.loads(arg)


def build_command(params: dict[str, Any]) -> list[str]:
    action = params.get("action", "render")
    if action not in ACTIONS:
        raise ValueError(f"unknown action: {action}")

    cmd = [sys.executable, str(RENDER)]

    if action == "examples":
        return cmd + ["--examples"]
    if action == "styles":
        return cmd + ["--styles"]

    example = params.get("example")
    path = params.get("path")
    if example:
        cmd += ["--example", str(example)]
    elif path:
        cmd.append(str(path))
    else:
        raise ValueError("path or example is required for this action")

    if action == "inspect":
        cmd.append("--inspect")
    elif action == "validate":
        cmd.append("--validate")
    elif action == "bounds":
        cmd.append("--bounds")

    for key, flag in BOOL_FLAGS.items():
        if params.get(key):
            cmd.append(flag)
    if "color" not in params:
        cmd += ["--color", "always"]

    for key, flag in VALUE_FLAGS.items():
        value = params.get(key)
        if value is not None and value != "":
            cmd += [flag, str(value)]

    return cmd


def main() -> int:
    try:
        params = read_params()
        action = params.get("action", "render")
        cmd = build_command(params)
        proc = subprocess.run(cmd, cwd=PROJECT_ROOT, text=True, capture_output=True, timeout=60)
        if proc.returncode != 0:
            print(
                json.dumps(
                    {
                        "success": False,
                        "action": action,
                        "command": cmd,
                        "error": (proc.stderr or proc.stdout).strip(),
                    },
                    separators=(",", ":"),
                )
            )
            return 1

        output_path = params.get("output")
        output = proc.stdout
        if output_path and action == "render":
            output = f"rendered diagram written to {output_path}"

        print(
            json.dumps(
                {"success": True, "action": action, "command": cmd, "output": output},
                separators=(",", ":"),
            )
        )
        return 0
    except Exception as e:
        print(json.dumps({"success": False, "error": str(e)}, separators=(",", ":")))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
