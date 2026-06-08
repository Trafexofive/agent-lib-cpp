#!/usr/bin/env python3
"""Run real MK3 scenarios in isolated workspaces.

This is not a prompt micro-test. It exercises agent work loops:
- protocol/harness compliance
- tool discovery
- code editing
- TDD / refactor behavior

Default mode is local process sandbox in a temp workspace. Docker mode can be
added without changing scenario specs.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCENARIO_ROOT = Path(__file__).resolve().parent
DEFAULT_RUNS = SCENARIO_ROOT / "runs"


def run(cmd, cwd, timeout, env=None):
    start = time.time()
    p = subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    return {
        "code": p.returncode,
        "stdout": p.stdout,
        "stderr": p.stderr,
        "elapsed": time.time() - start,
        "cmd": cmd,
    }


def copy_fixture(fixture, dst):
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    if fixture:
        shutil.copytree(SCENARIO_ROOT / fixture, dst, dirs_exist_ok=True)


def write_result(path, result):
    path.write_text(
        "$ " + " ".join(result["cmd"]) + "\n"
        + f"# exit={result['code']} elapsed={result['elapsed']:.2f}s\n\n"
        + "## STDOUT\n" + result["stdout"] + "\n\n"
        + "## STDERR\n" + result["stderr"] + "\n"
    )


def run_scenario(scenario, args):
    run_id = time.strftime("%Y%m%d-%H%M%S")
    out_dir = args.output / f"{scenario['id']}-{run_id}"
    workspace = out_dir / "workspace"
    out_dir.mkdir(parents=True, exist_ok=True)
    copy_fixture(scenario.get("fixture"), workspace)

    print(f"\n════════════════════════════════════════════════════════════")
    print(f"scenario: {scenario['id']}  tier={scenario.get('tier')}  model={args.model}")
    print(f"workspace: {workspace}")
    print(f"prompt: {scenario['prompt']}")
    print("════════════════════════════════════════════════════════════")

    verify = scenario.get("verify")
    if verify:
        before = run(["bash", "-lc", verify], workspace, args.verify_timeout)
        write_result(out_dir / "verify-before.txt", before)
        print(f"before verify: exit={before['code']} ({before['elapsed']:.1f}s)")
        print("--- before stdout/stderr preview ---")
        print((before["stdout"] + before["stderr"]).strip().split("\n")[-8:])

    env = os.environ.copy()
    cmd = [
        str(ROOT / "cortex-mk3"),
        "--raw",
        "--provider", args.provider,
        "--model", args.model,
        "--manifest-dir", str(ROOT / "manifests"),
        "--manifest", str(SCENARIO_ROOT / "agent.yml"),
        "--harness", str(ROOT / "config/harness/default.md"),
    ]
    if args.sandbox:
        cmd.append("--sandbox")
    cmd += ["run", "-p", scenario["prompt"]]

    result = run(cmd, workspace, args.timeout, env=env)
    write_result(out_dir / "agent-run.txt", result)
    print(f"agent: exit={result['code']} ({result['elapsed']:.1f}s)")
    print("--- agent stdout preview ---")
    print("\n".join(result["stdout"].strip().split("\n")[:30]))
    if result["stderr"].strip():
        print("--- agent stderr preview ---")
        print("\n".join(result["stderr"].strip().split("\n")[:20]))

    # Capture protocol dumps if produced in workspace.
    for name in ["raw.md", "iterations.md"]:
        src = workspace / name
        if src.exists():
            shutil.copy2(src, out_dir / name)
            if name == "iterations.md" and (ROOT / "tests/context-filter").exists():
                filt = run([str(ROOT / "tests/context-filter"), str(src), "--only-agent"], workspace, 10)
                (out_dir / "agent-turns.txt").write_text(filt["stdout"] + filt["stderr"])

    if verify:
        after = run(["bash", "-lc", verify], workspace, args.verify_timeout)
        write_result(out_dir / "verify-after.txt", after)
        print(f"after verify: exit={after['code']} ({after['elapsed']:.1f}s)")
        print("--- after stdout/stderr preview ---")
        print((after["stdout"] + after["stderr"]).strip().split("\n")[-10:])

    print(f"saved: {out_dir}")
    return out_dir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", action="append", help="Scenario id to run; repeatable")
    ap.add_argument("--tier", help="Run all scenarios in tier")
    ap.add_argument("--provider", default=os.getenv("CORTEX_PROVIDER", "deepseek"))
    ap.add_argument("--model", default=os.getenv("CORTEX_MODEL", "deepseek-v4-pro"))
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--verify-timeout", type=int, default=30)
    ap.add_argument("--no-sandbox", dest="sandbox", action="store_false")
    ap.set_defaults(sandbox=True)
    ap.add_argument("--output", type=Path, default=DEFAULT_RUNS)
    args = ap.parse_args()

    scenarios = json.loads((SCENARIO_ROOT / "scenarios.json").read_text())
    selected = []
    for s in scenarios:
        if args.scenario and s["id"] not in args.scenario:
            continue
        if args.tier and s.get("tier") != args.tier:
            continue
        selected.append(s)
    if not selected:
        print("No scenarios selected", file=sys.stderr)
        return 2

    for s in selected:
        run_scenario(s, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
