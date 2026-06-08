#!/usr/bin/env python3
"""diff_analyze — Analyze a git diff for structured code review."""
import json
import os
import subprocess
import sys
import re

def run_git(args, cwd):
    """Run a git command, return (exit_code, output)."""
    try:
        r = subprocess.run(
            ["git"] + args, cwd=cwd, capture_output=True, text=True, timeout=30
        )
        output = r.stdout
        if r.stderr:
            output += "\n" + r.stderr
        return r.returncode, output.strip()
    except Exception as e:
        return -1, str(e)

def parse_stat(stat_output):
    """Parse git diff --stat output into structured data."""
    files = []
    total_insertions = 0
    total_deletions = 0
    for line in stat_output.split("\n"):
        # Match: "  path/to/file.py | 5 ++--"
        m = re.match(r'\s+(.+?)\s+\|\s+(\d+)', line)
        if m:
            path = m.group(1).strip()
            changes = int(m.group(2))
            # Count + and - in the line
            plus = line.count('+')
            minus = line.count('-')
            files.append({
                "path": path,
                "changes": changes,
                "insertions": plus,
                "deletions": minus,
            })
            total_insertions += plus
            total_deletions += minus
    return files, total_insertions, total_deletions

def main():
    try:
        params = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    except (json.JSONDecodeError, ValueError) as e:
        print(json.dumps({"error": f"invalid JSON input: {e}"}))
        sys.exit(1)
    path = os.path.abspath(params.get("path", "."))
    target = params.get("target", "HEAD")
    flags = params.get("flags", "")

    if not os.path.isdir(path):
        print(json.dumps({"error": f"not a directory: {path}"}))
        sys.exit(1)

    # Check if inside a git repo
    exit_code, _ = run_git(["rev-parse", "--is-inside-work-tree"], path)
    if exit_code != 0:
        print(json.dumps({"error": "not a git repository"}))
        sys.exit(1)

    # Build diff command
    if target == "staged":
        diff_args = ["diff", "--staged"]
    else:
        diff_args = ["diff", target]

    if flags:
        diff_args.extend(flags.split())

    # Get stat summary
    _, stat_output = run_git(diff_args + ["--stat"], path)
    files, insertions, deletions = parse_stat(stat_output)

    # Get full diff
    _, diff_output = run_git(diff_args, path)
    # Truncate if massive
    diff_truncated = len(diff_output) > 16000
    diff_content = diff_output[:16000] if diff_truncated else diff_output

    result = {
        "files_changed": len(files),
        "insertions": insertions,
        "deletions": deletions,
        "files": files,
        "diff": diff_content,
        "diff_truncated": diff_truncated,
    }

    print(json.dumps(result))

if __name__ == "__main__":
    main()
