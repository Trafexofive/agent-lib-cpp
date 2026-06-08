#!/usr/bin/env python3
"""Diff-analyze tool entrypoint — compare two plan versions."""
import sys, json, os

sys.path.insert(0, os.path.dirname(__file__))
from diff.engine import diff_plans
from diff.parser import normalize
from report.formatter import format_diff


def load_params() -> dict:
    raw = os.environ.get("TOOL_PARAMS", "")
    if raw:
        return json.loads(raw)
    if len(sys.argv) > 1:
        return json.loads(sys.argv[1])
    return {}


def main(params: dict) -> dict:
    plan_a = normalize(params.get("plan_a", []))
    plan_b = normalize(params.get("plan_b", []))

    if not plan_a or not plan_b:
        return {"success": False, "error": "Both plan_a and plan_b are required"}

    diff = diff_plans(plan_a, plan_b)
    return {
        "success": True,
        **diff,
        "summary": format_diff(diff),
    }


if __name__ == "__main__":
    params = load_params()
    result = main(params)
    print(json.dumps(result))
