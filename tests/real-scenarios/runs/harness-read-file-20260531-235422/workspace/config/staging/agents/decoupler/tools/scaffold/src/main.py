#!/usr/bin/env python3
"""
Scaffold tool entrypoint — decompose tasks into sub-task plans.

Reads params from TOOL_PARAMS env var (JSON), falls back to argv[1].
Outputs JSON to stdout.
"""
import sys, json, os

sys.path.insert(0, os.path.dirname(__file__))

from analysis.extractor import extract_concerns
from analysis.classifier import classify_subtask
from planning.dag import build_dag
from planning.scheduler import schedule
from validation.integrity import validate


def load_params() -> dict:
    """Params come via TOOL_PARAMS env var (set by executeScriptTool)."""
    raw = os.environ.get("TOOL_PARAMS", "")
    if raw:
        return json.loads(raw)
    if len(sys.argv) > 1:
        return json.loads(sys.argv[1])
    return {}


def main(params: dict) -> dict:
    task = params.get("task", "")
    mode = params.get("mode", "decompose")

    if not task:
        return {"success": False, "error": "No task provided"}

    if mode == "decompose":
        sub_tasks = extract_concerns(task)
        sub_tasks = [classify_subtask(st) for st in sub_tasks]
        plan = build_dag(sub_tasks)
        ordered = schedule(plan)
        valid, warnings = validate(ordered)
        return {
            "success": True,
            "plan": ordered,
            "valid": valid,
            "warnings": warnings,
            "task_count": len(ordered),
        }

    if mode == "validate":
        plan_input = params.get("plan", [])
        valid, warnings = validate(plan_input)
        return {
            "success": True,
            "valid": valid,
            "warnings": warnings,
        }

    return {"success": False, "error": f"Unknown mode: {mode}"}


if __name__ == "__main__":
    params = load_params()
    result = main(params)
    print(json.dumps(result))
