#!/usr/bin/env python3
"""Test runner — auto-detects and executes test suites with structured output."""

import sys
import json
import subprocess
import os
import re

def detect_test_command(work_dir):
    """Auto-detect the test command based on project files."""
    files = os.listdir(work_dir) if os.path.isdir(work_dir) else []

    if "Makefile" in files:
        # Check for test targets
        try:
            result = subprocess.run(
                ["make", "-n", "test"],
                capture_output=True, text=True, cwd=work_dir, timeout=5
            )
            if result.returncode == 0:
                return "make test", "make"
        except Exception:
            pass

    if "Cargo.toml" in files:
        return "cargo test", "cargo"

    if "go.mod" in files:
        return "go test ./...", "go"

    if "package.json" in files:
        return "npm test", "npm"

    if "pytest.ini" in files or os.path.exists(os.path.join(work_dir, "tests")):
        return "pytest -v", "pytest"

    # Check for common test binaries
    for binary in ["runtime-test", "test", "run_tests", "tests"]:
        path = os.path.join(work_dir, binary)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return f"./{binary}", "custom"

    return None, None

def run_tests(command=None, filter_name=None, verbose=True, working_dir=None):
    """Execute the test suite and parse results."""
    work_dir = working_dir or os.getcwd()
    framework = "unknown"

    if command:
        test_cmd = command
        framework = "custom"
    else:
        test_cmd, framework = detect_test_command(work_dir)
        if not test_cmd:
            return json.dumps({
                "error": "No test framework detected",
                "framework": "unknown",
                "suggestion": "Specify a custom command parameter"
            })

    # Add filter if specified
    if filter_name:
        if "pytest" in test_cmd:
            test_cmd += f" -k '{filter_name}'"
        elif "cargo" in test_cmd:
            test_cmd += f" {filter_name}"
        elif "go" in test_cmd:
            test_cmd += f" -run {filter_name}"
        else:
            test_cmd += f" {filter_name}"

    # Run the tests
    try:
        result = subprocess.run(
            test_cmd,
            shell=True,
            capture_output=True,
            text=True,
            cwd=work_dir,
            timeout=120
        )
        output = result.stdout + result.stderr
        exit_code = result.returncode
    except subprocess.TimeoutExpired:
        return json.dumps({
            "error": "Test execution timed out (120s)",
            "framework": framework,
            "command": test_cmd
        })
    except Exception as e:
        return json.dumps({
            "error": str(e),
            "framework": framework,
            "command": test_cmd
        })

    # Parse results
    total, passed, failed = 0, 0, 0
    failures = []

    # Try to parse common test output formats
    # Make/CMake format: "X% tests passed, Y tests failed out of Z"
    make_match = re.search(r'(\d+)% tests passed.*?(\d+) tests failed.*?out of (\d+)', output)
    if make_match:
        passed_pct = int(make_match.group(1))
        failed = int(make_match.group(2))
        total = int(make_match.group(3))
        passed = total - failed

    # pytest format: "X passed, Y failed, Z warnings"
    pytest_match = re.search(r'(\d+) passed.*?(\d+) failed', output)
    if pytest_match and not make_match:
        passed = int(pytest_match.group(1))
        failed = int(pytest_match.group(2))
        total = passed + failed

    # cargo format: "test result: ok. X passed; Y failed; Z ignored"
    cargo_match = re.search(r'(\d+) passed.*?(\d+) failed', output)
    if cargo_match and not make_match and not pytest_match:
        passed = int(cargo_match.group(1))
        failed = int(cargo_match.group(2))
        total = passed + failed

    # Go format: "ok  pkg  X.YYYs  PASS/FAIL"
    go_matches = re.findall(r'(PASS|FAIL)\s', output)
    if go_matches and not make_match and not pytest_match and not cargo_match:
        passed = sum(1 for r in go_matches if r == "PASS")
        failed = sum(1 for r in go_matches if r == "FAIL")
        total = passed + failed

    # Extract failure details
    if failed > 0:
        # Look for FAIL/FAILED lines
        for line in output.split('\n'):
            line = line.strip()
            if re.search(r'FAIL|FAILED|ERROR|Assertion', line, re.IGNORECASE):
                if len(line) < 500:  # Don't include huge lines
                    failures.append(line)
                if len(failures) >= 20:  # Cap at 20 failures
                    break

    return json.dumps({
        "framework": framework,
        "command": test_cmd,
        "total": total,
        "passed": passed,
        "failed": failed,
        "exit_code": exit_code,
        "success": exit_code == 0,
        "failures": failures[:20],
        "output": output if verbose else output[-2000:]  # Truncate if not verbose
    }, indent=2)

if __name__ == "__main__":
    try:
        params = json.loads(sys.stdin.read())
    except json.JSONDecodeError:
        print(json.dumps({"error": "Invalid JSON input"}))
        sys.exit(1)

    command = params.get("command", "")
    filter_name = params.get("filter", "")
    verbose = params.get("verbose", True)
    working_dir = params.get("working_dir", "")

    result = run_tests(
        command=command or None,
        filter_name=filter_name or None,
        verbose=verbose,
        working_dir=working_dir or None
    )
    print(result)
