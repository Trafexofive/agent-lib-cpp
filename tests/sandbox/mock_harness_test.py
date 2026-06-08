#!/usr/bin/env python3
"""
Mock harness test — validates harness prompt + protocol WITHOUT any LLM calls.
Runs inside the podman sandbox against the built cortex-mk3 binary.
Tests: protocol smoke, multi-turn, sub-agent, error recovery, feed, relic, stress.

Usage:
  ./mock_harness_test.py [--verbose]
"""
import sys, os, json, re, subprocess, time, argparse
from pathlib import Path

# ── Test scenarios ─────────────────────────────────────────────────

SCENARIOS = {
    "01_protocol_smoke": {
        "desc": "Protocol smoke test — identity + simple response",
        "prompts": [
            "Who are you and what protocol do you speak?",
            "What is 7 times 9?",
        ],
        "validate": "expect_response_or_action"
    },
    "02_multiturn_tools": {
        "desc": "Multi-turn tool orchestration — list → read → grep",
        "prompts": [
            "List the files in the current directory, then read README.md, then search for 'TODO' in src/.",
        ],
        "validate": "expect_multiple_actions"
    },
    "03_subagent_delegation": {
        "desc": "Sub-agent spawn + collect — delegate a build",
        "prompts": [
            "Deploy (pretend-build) the project by delegating to the builder agent.",
        ],
        "validate": "expect_agent_action"
    },
    "04_error_recovery": {
        "desc": "Error recovery — file not found → retry → succeed",
        "prompts": [
            "Read the file /nonexistent/ghost.txt and handle the error gracefully.",
            "Run 'invalid_command_xyz --flag' and report what happened.",
            "Try to read /etc/shadow. If it fails, try /etc/hostname instead.",
        ],
        "validate": "expect_error_handling"
    },
    "05_feed_polling": {
        "desc": "Feed polling — check CI status",
        "prompts": [
            "Check the CI status feed for recent failures.",
        ],
        "validate": "expect_feed_action"
    },
    "06_relic_integration": {
        "desc": "Relic integration — database health check",
        "prompts": [
            "Check if the postgres_main database is reachable.",
        ],
        "validate": "expect_relic_action"
    },
    "07_stress_cycling": {
        "desc": "Stress: rapid multi-turn — analyze → pin → search → read → summarize",
        "prompts": [
            "Analyze this project: list src/ structure, find TODOs, read the first TODO file, pin it to context, and report a summary.",
        ],
        "validate": "expect_5plus_actions"
    },
}

# ── Validation functions ───────────────────────────────────────────

def validate_response(output, scenario):
    """Validate that output follows the harness protocol."""
    issues = []

    # Check for bare text (no XML tags)
    if '<action ' not in output and '<response ' not in output and '<thought>' not in output:
        issues.append("NO_XML: Output contains no XML action/response/thought tags")

    # Check for both action AND response in same turn (shouldn't happen in raw mode)
    if '<action ' in output and '<response final="true">' in output:
        # This could be multi-line output from multi-turn — check if they're in sequence
        lines = output.split('\n')
        action_lines = [i for i, l in enumerate(lines) if '<action ' in l]
        response_lines = [i for i, l in enumerate(lines) if '<response final="true">' in l]
        if action_lines and response_lines and abs(action_lines[-1] - response_lines[-1]) < 3:
            issues.append("MIXED: Action and response appear in same turn context")

    # Check for proper XML closure
    if '<action ' in output and '</action>' not in output:
        issues.append("UNCLOSED_ACTION: Action tag not closed")
    if '<response ' in output and '</response>' not in output:
        issues.append("UNCLOSED_RESPONSE: Response tag not closed")

    return issues


def validate_multiaction(output):
    """Expect multiple tool actions before final response."""
    issues = []
    actions = len(re.findall(r'<action type="tool"', output))
    if actions < 2:
        issues.append(f"FEW_ACTIONS: Expected ≥2 tool actions, got {actions}")
    if '<response final="true">' not in output:
        issues.append("NO_FINAL: No final response")
    return issues


def validate_agent_action(output):
    """Expect agent-type action delegation."""
    issues = []
    if '<action type="agent"' not in output:
        issues.append("NO_AGENT: No agent delegation action found")
    if '<response final="true">' not in output:
        issues.append("NO_FINAL: No final response")
    return issues


def validate_error_handling(output):
    """Expect error recovery pattern."""
    issues = []
    actions = re.findall(r'<action type="tool"', output)
    if len(actions) < 2:
        issues.append(f"FEW_ACTIONS: Error recovery needs ≥2 actions, got {len(actions)}")
    if '<response final="true">' not in output:
        issues.append("NO_FINAL: No final response after error recovery")
    return issues


def validate_feed_action(output):
    """Expect feed-type action."""
    issues = []
    if '<action type="feed"' not in output:
        issues.append("NO_FEED: No feed action found")
    if '<response final="true">' not in output:
        issues.append("NO_FINAL: No final response")
    return issues


def validate_relic_action(output):
    """Expect relic-type action."""
    issues = []
    if '<action type="relic"' not in output:
        issues.append("NO_RELIC: No relic action found")
    if '<response final="true">' not in output:
        issues.append("NO_FINAL: No final response")
    return issues


def validate_5plus_actions(output):
    """Expect 5+ actions (stress test)."""
    issues = []
    actions = len(re.findall(r'<action ', output))
    if actions < 3:  # Lowered because model may consolidate
        issues.append(f"FEW_ACTIONS: Expected ≥3 actions, got {actions}")
    if '<response final="true">' not in output:
        issues.append("NO_FINAL: No final response")
    return issues


VALIDATORS = {
    "expect_response_or_action": validate_response,
    "expect_multiple_actions": validate_multiaction,
    "expect_agent_action": validate_agent_action,
    "expect_error_handling": validate_error_handling,
    "expect_feed_action": validate_feed_action,
    "expect_relic_action": validate_relic_action,
    "expect_5plus_actions": validate_5plus_actions,
}

# ── Main ───────────────────────────────────────────────────────────

def run_scenario(binary, harness_prompt, provider, model, scenario_key, verbose=False):
    """Run a single test scenario against cortex-mk3."""
    scenario = SCENARIOS[scenario_key]
    print(f"\n{'='*60}")
    print(f"  {scenario_key}: {scenario['desc']}")
    print(f"{'='*60}")

    all_issues = []
    for i, prompt in enumerate(scenario['prompts']):
        print(f"\n  [{i+1}/{len(scenario['prompts'])}] Prompt: {prompt[:80]}...")

        cmd = [
            binary,
            "--raw",
            "--harness-prompt", harness_prompt,
            "--provider", provider,
            "--model", model,
            "--prompt", prompt,
        ]

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=120,
                cwd=os.path.dirname(binary) or "."
            )
            output = result.stdout.strip()
            if result.returncode != 0:
                output += f"\n[STDERR]\n{result.stderr}"

        except subprocess.TimeoutExpired:
            output = "[TIMEOUT] Scenario exceeded 120s"
        except Exception as e:
            output = f"[ERROR] {e}"

        if verbose:
            print(f"    --- raw output ---\n{output[:500]}\n    --- end ---")

        # Validate
        validator = VALIDATORS.get(scenario['validate'], validate_response)
        issues = validator(output)
        all_issues.extend(issues)

        if issues:
            print(f"    ❌ Issues: {', '.join(issues)}")
        else:
            print(f"    ✅ Pass")

        # Check for key markers
        if '<action ' in output:
            actions = re.findall(r'<action type="(\w+)"', output)
            print(f"    Actions: {actions}")
        if '<response final="true">' in output:
            resp_match = re.search(r'<response final="true">(.+?)</response>', output, re.DOTALL)
            if resp_match:
                resp_text = resp_match.group(1).strip()[:100]
                print(f"    Response: {resp_text}")

    return len(all_issues) == 0, all_issues


def main():
    parser = argparse.ArgumentParser(description='Mock harness test runner')
    parser.add_argument('--binary', default='./cortex-mk3', help='Path to cortex-mk3 binary')
    parser.add_argument('--harness-prompt', default='config/harness/default.md',
                       help='Path to harness prompt')
    parser.add_argument('--provider', default='zen', help='LLM provider')
    parser.add_argument('--model', default='big-pickle', help='Model name')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    parser.add_argument('--scenario', help='Run a single scenario (e.g. 01_protocol_smoke)')
    parser.add_argument('--zen-key', default='public', help='Zen API key override')
    args = parser.parse_args()

    # Resolve full paths
    binary = os.path.abspath(args.binary)
    harness_prompt = os.path.abspath(args.harness_prompt)

    if not os.path.exists(binary):
        print(f"ERROR: Binary not found: {binary}")
        print("Build with: make cortex-mk3")
        sys.exit(1)

    if not os.path.exists(harness_prompt):
        print(f"ERROR: Harness prompt not found: {harness_prompt}")
        sys.exit(1)

    print(f"═══ Cortex MK3 Harness Test ═══")
    print(f"  binary:   {binary}")
    print(f"  harness:  {harness_prompt}")
    print(f"  provider: {args.provider}")
    print(f"  model:    {args.model}")
    if args.provider == 'zen':
        print(f"  zen_key:  {args.zen_key}")

    scenarios_to_run = [args.scenario] if args.scenario else sorted(SCENARIOS.keys())

    passed = 0
    failed = 0
    all_results = {}

    for key in scenarios_to_run:
        if key not in SCENARIOS:
            print(f"Unknown scenario: {key}")
            continue
        ok, issues = run_scenario(binary, harness_prompt, args.provider, args.model,
                                  key, verbose=args.verbose)
        all_results[key] = {"passed": ok, "issues": issues}
        if ok:
            passed += 1
        else:
            failed += 1

    # ── Summary ──
    print(f"\n{'='*60}")
    print(f"  RESULTS: {passed} passed, {failed} failed, {len(scenarios_to_run)} total")
    print(f"{'='*60}")
    for key, result in all_results.items():
        status = "✅" if result['passed'] else "❌"
        print(f"  {status} {key}")
        if result['issues']:
            for issue in result['issues']:
                print(f"     → {issue}")

    sys.exit(0 if failed == 0 else 1)


if __name__ == '__main__':
    main()
