#!/usr/bin/env python3
"""Recursive directory tree tool. Readonly. Gitignore-aware."""
from __future__ import annotations
import fnmatch
import json
import sys
from pathlib import Path
from typing import Any

DEFAULT_EXCLUDES = [
    ".git", "__pycache__", "node_modules", ".venv", "venv",
    "*.pyc", ".DS_Store", "dist", "build", ".cache",
]


def read_params() -> dict[str, Any]:
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        return {}
    arg = sys.argv[1]
    if arg.startswith("/") or arg.startswith("./") or arg.startswith("../"):
        try:
            return json.loads(Path(arg).read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            pass
    return json.loads(arg)


def load_gitignore(root: Path) -> list[str]:
    patterns: list[str] = []
    gi = root / ".gitignore"
    if gi.is_file():
        for line in gi.read_text(errors="ignore").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                patterns.append(line.rstrip("/"))
    return patterns


def is_excluded(name: str, rel_path: str, patterns: list[str]) -> bool:
    for pat in patterns:
        if fnmatch.fnmatch(name, pat) or fnmatch.fnmatch(rel_path, pat):
            return True
    return False


def scan(
    root: Path,
    max_depth: int,
    include_hidden: bool,
    exclude_patterns: list[str],
    extensions: list[str] | None,
    max_entries: int,
) -> dict[str, Any]:
    stats = {"dirs": 0, "files": 0, "total_size": 0}
    truncated = False
    entry_count = 0

    def walk(dir_path: Path, depth: int) -> dict[str, Any] | None:
        nonlocal truncated, entry_count
        node: dict[str, Any] = {"name": dir_path.name or str(dir_path), "type": "dir", "children": []}
        if depth > max_depth:
            node["truncated"] = "max_depth"
            return node
        try:
            children = sorted(dir_path.iterdir(), key=lambda p: (p.is_file(), p.name.lower()))
        except PermissionError:
            node["error"] = "permission_denied"
            return node

        for child in children:
            if entry_count >= max_entries:
                truncated = True
                break
            if not include_hidden and child.name.startswith("."):
                continue
            rel = str(child.relative_to(root))
            if is_excluded(child.name, rel, exclude_patterns):
                continue

            if child.is_dir():
                stats["dirs"] += 1
                entry_count += 1
                sub = walk(child, depth + 1)
                if sub is not None:
                    node["children"].append(sub)
            elif child.is_file():
                if extensions and child.suffix.lstrip(".") not in extensions:
                    continue
                stats["files"] += 1
                entry_count += 1
                try:
                    size = child.stat().st_size
                except OSError:
                    size = 0
                stats["total_size"] += size
                node["children"].append({"name": child.name, "type": "file", "size": size})
        return node

    tree = walk(root, 0)
    return {"tree": tree, "stats": stats, "truncated": truncated}


def render_ascii(node: dict[str, Any], prefix: str = "", is_last: bool = True, lines: list[str] | None = None) -> list[str]:
    if lines is None:
        lines = [node.get("name", "?")]
        children = node.get("children", [])
        for i, child in enumerate(children):
            render_ascii(child, "", i == len(children) - 1, lines)
        return lines

    connector = "└── " if is_last else "├── "
    suffix = ""
    if node["type"] == "file":
        suffix = f" ({node.get('size', 0)}B)"
    elif node.get("truncated"):
        suffix = " [depth limit]"
    elif node.get("error"):
        suffix = f" [{node['error']}]"
    lines.append(f"{prefix}{connector}{node['name']}{suffix}")

    if node["type"] == "dir":
        ext = "    " if is_last else "│   "
        children = node.get("children", [])
        for i, child in enumerate(children):
            render_ascii(child, prefix + ext, i == len(children) - 1, lines)
    return lines


def main() -> int:
    try:
        params = read_params()
        path = params.get("path", ".")
        root = Path(path).resolve()
        if not root.is_dir():
            print(json.dumps({"success": False, "error": f"not a directory: {path}"}))
            return 1

        max_depth = int(params.get("max_depth", 6))
        include_hidden = bool(params.get("include_hidden", False))
        respect_gitignore = bool(params.get("gitignore", True))
        extensions = params.get("extensions")
        if extensions:
            extensions = [e.lstrip(".") for e in extensions]
        max_entries = int(params.get("max_entries", 2000))
        output_format = params.get("format", "tree")

        exclude_patterns = list(DEFAULT_EXCLUDES) + list(params.get("exclude", []))
        if respect_gitignore:
            exclude_patterns += load_gitignore(root)

        result = scan(root, max_depth, include_hidden, exclude_patterns, extensions, max_entries)

        out: dict[str, Any] = {
            "success": True,
            "root": str(root),
            "stats": result["stats"],
            "truncated": result["truncated"],
        }
        if output_format == "json":
            out["tree"] = result["tree"]
        else:
            out["tree"] = "\n".join(render_ascii(result["tree"]))

        print(json.dumps(out, separators=(",", ":")))
        return 0
    except Exception as e:
        print(json.dumps({"success": False, "error": str(e)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())