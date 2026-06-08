#!/usr/bin/env python3
"""build_run — Detect project type, build, and run tests."""
import json
import os
import subprocess
import sys

PROJECT_RULES = [
    # (detector_file, build_cmd, test_cmd, clean_cmd, project_type)
    ("Makefile",      "make -j$(nproc)",  "make test",            "rm -rf build",         "make"),
    ("Cargo.toml",    "cargo build",      "cargo test",           "cargo clean",           "rust"),
    ("go.mod",        "go build ./...",   "go test ./...",        None,                    "go"),
    ("package.json",  "npm run build",    "npm test",             "rm -rf node_modules",   "npm"),
    ("pom.xml",       "mvn compile",      "mvn test",             "mvn clean",             "maven"),
    ("build.gradle",  "gradle build",     "gradle test",          "gradle clean",          "gradle"),
]

def run(cmd, cwd, timeout=120):
    """Run a command, return (exit_code, output)."""
    try:
        r = subprocess.run(
            cmd, shell=True, cwd=cwd, capture_output=True, text=True, timeout=timeout
        )
        output = r.stdout
        if r.stderr:
            output += "\n" + r.stderr
        return r.returncode, output.strip()
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"
    except Exception as e:
        return -1, str(e)

def main():
    try:
        params = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    except (json.JSONDecodeError, ValueError) as e:
        print(json.dumps({"error": f"invalid JSON input: {e}"}))
        sys.exit(1)
    path = os.path.abspath(params.get("path", "."))
    do_clean = params.get("clean", False)
    target = params.get("target", "")

    if not os.path.isdir(path):
        print(json.dumps({"error": f"not a directory: {path}"}))
        sys.exit(1)

    # Detect project type
    detected = None
    for detector, build_cmd, test_cmd, clean_cmd, ptype in PROJECT_RULES:
        if os.path.exists(os.path.join(path, detector)):
            detected = (detector, build_cmd, test_cmd, clean_cmd, ptype)
            break

    if not detected:
        print(json.dumps({"error": "unknown project type", "project_type": None}))
        sys.exit(1)

    _, build_cmd, test_cmd, clean_cmd, ptype = detected
    result = {"project_type": ptype}

    # Clean if requested
    if do_clean and clean_cmd:
        _, clean_output = run(clean_cmd, path)
        result["clean_output"] = clean_output

    # Override test command if target specified
    if target:
        test_cmd = target

    # Build
    build_exit, build_output = run(build_cmd, path)
    result["build_exit_code"] = build_exit
    result["build_output"] = build_output[-4000:] if len(build_output) > 4000 else build_output

    # Test (only if build succeeded)
    if build_exit == 0:
        test_exit, test_output = run(test_cmd, path, timeout=180)
        result["test_exit_code"] = test_exit
        result["test_output"] = test_output[-4000:] if len(test_output) > 4000 else test_output
    else:
        result["test_exit_code"] = -1
        result["test_output"] = "skipped — build failed"

    print(json.dumps(result))

if __name__ == "__main__":
    main()
