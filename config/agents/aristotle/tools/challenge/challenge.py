#!/usr/bin/env python3
"""
challenge.py — local script tool for the aristotle agent.

Walks a file and surfaces things a skeptic would doubt:
  - Unjustified assertion-words in comments ("always", "never", "safe",
    "obviously", "clearly", "guaranteed", "impossible", "must")
  - TODO / FIXME / HACK / XXX markers
  - Unchecked errors (`if (!x) return;` with no comment)
  - Magic numbers (numeric literals >= 10 not in a standard-size whitelist)

Reads params as JSON from argv[1]. Prints a JSON envelope to stdout:
  {"success": true, "path": ..., "lines_scanned": N, "count": N, "findings": [...]}

This tool is intentionally permissive: it over-includes. The caller (an LLM
persona, namely aristotle) triages which findings are real.
"""

import json
import os
import re
import sys


ASSERTION_WORDS = [
    "always", "never", "safe", "obviously", "clearly", "guaranteed",
    "impossible", "must", "trivial", "of course",
]
TODO_MARKERS = ["TODO", "FIXME", "HACK", "XXX", "BUG"]
# Sizes we don't flag as "magic numbers" — page sizes, common buffers, etc.
STANDARD_SIZES = {0, 1, 2, 8, 10, 16, 24, 32, 64, 100, 128, 256, 512, 1000,
                  1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144}

GUARD_RE = re.compile(r"if\s*\(\s*!\s*[A-Za-z_][A-Za-z_0-9]*\s*\)\s*(return|continue|break)\s*;")
ASSERT_MACRO_RE = re.compile(r"\bassert\s*\(")
NUM_RE = re.compile(r"(\d{2,})")


def trim(s: str) -> str:
    return s.strip()


def strip_comment_delim(s: str) -> str:
    t = s
    if t.startswith("//"):
        t = t[2:]
    elif t.startswith("/*"):
        t = t[2:]
    if t.endswith("*/"):
        t = t[:-2]
    return t.strip()


def line_is_comment(s: str) -> bool:
    t = trim(s)
    return t.startswith("//") or t.startswith("/*")


def contains_assertion_word(lower: str) -> bool:
    for w in ASSERTION_WORDS:
        for m in re.finditer(r"\b" + re.escape(w), lower):
            return True
    return False


def contains_todo(t: str) -> bool:
    for m in TODO_MARKERS:
        for match in re.finditer(r"\b" + m, t):
            return True
    return False


def is_unchecked_guard(t: str) -> bool:
    return bool(GUARD_RE.search(t))


def is_assert_macro(t: str) -> bool:
    return bool(ASSERT_MACRO_RE.search(t))


def find_magic_numbers(line: str) -> bool:
    for m in NUM_RE.finditer(line):
        pos = m.start()
        # Skip if preceded by alnum/_/. (part of a larger token)
        if pos > 0 and (line[pos - 1].isalnum() or line[pos - 1] in "_."):
            continue
        try:
            v = int(m.group(1))
        except ValueError:
            continue
        if v >= 10 and v not in STANDARD_SIZES:
            return True
    return False


def challenge_file(path: str, max_findings: int = 50) -> dict:
    findings = []
    if not os.path.isfile(path):
        return {"success": False, "error": f"file not found: {path}"}

    with open(path, "r", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            t = trim(line)
            if not t:
                continue
            lower = t.lower()

            if line_is_comment(t):
                body = strip_comment_delim(t)
                if contains_assertion_word(body):
                    findings.append({"line": lineno, "kind": "assertion",
                                      "severity": "concern", "evidence": t})
                elif contains_todo(t):
                    findings.append({"line": lineno, "kind": "todo",
                                      "severity": "nit", "evidence": t})
                continue

            if is_unchecked_guard(t):
                findings.append({"line": lineno, "kind": "unchecked",
                                  "severity": "concern", "evidence": t})
            if is_assert_macro(t):
                findings.append({"line": lineno, "kind": "assert_macro",
                                  "severity": "nit", "evidence": t})
            if find_magic_numbers(t):
                findings.append({"line": lineno, "kind": "magic",
                                  "severity": "nit", "evidence": t})

            if len(findings) >= max_findings:
                break

    return {
        "success": True,
        "path": path,
        "lines_scanned": lineno,
        "count": len(findings),
        "findings": findings,
    }


def main():
    if len(sys.argv) < 2:
        print(json.dumps({"success": False, "error": "missing params"}))
        sys.exit(1)
    try:
        params = json.loads(sys.argv[1])
    except json.JSONDecodeError as e:
        print(json.dumps({"success": False, "error": f"bad params: {e}"}))
        sys.exit(1)
    path = params.get("path", "")
    max_findings = int(params.get("max_findings", 50))
    print(json.dumps(challenge_file(path, max_findings)))


if __name__ == "__main__":
    main()
