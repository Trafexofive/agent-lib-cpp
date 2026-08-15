#!/usr/bin/env python3
"""Squeezer: strips function/method bodies, keeps signatures/imports/types.
Produces a cheap 'Functional Map' of a file or directory tree.

Python uses ast (exact). Other languages use regex heuristics and are
tagged best_effort: true in output so downstream agents can weight
confidence accordingly.
"""
from __future__ import annotations
import ast
import json
import re
import sys
from pathlib import Path
from typing import Any

EXT_LANG = {
    ".py": "python", ".c": "c", ".h": "c", ".cpp": "cpp", ".hpp": "cpp",
    ".cc": "cpp", ".cxx": "cpp", ".js": "javascript", ".jsx": "javascript",
    ".ts": "typescript", ".tsx": "typescript", ".go": "go",
}
DEFAULT_EXCLUDES = {".git", "__pycache__", "node_modules", ".venv", "venv", "dist", "build"}


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


def squeeze_python(src: str) -> dict[str, Any]:
    tree = ast.parse(src)
    imports: list[str] = []
    signatures: list[str] = []

    def sig_of(node, prefix: str = "") -> str:
        kind = "async def" if isinstance(node, ast.AsyncFunctionDef) else "def"
        try:
            args = ast.unparse(node.args)
        except Exception:
            args = "..."
        ret = f" -> {ast.unparse(node.returns)}" if node.returns else ""
        return f"{prefix}{kind} {node.name}({args}){ret}"

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imports.append("import " + ", ".join(a.name for a in node.names))
        elif isinstance(node, ast.ImportFrom):
            mod = node.module or ""
            imports.append(f"from {'.' * node.level}{mod} import " + ", ".join(a.name for a in node.names))

    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            signatures.append(sig_of(node))
        elif isinstance(node, ast.ClassDef):
            bases = ", ".join(ast.unparse(b) for b in node.bases) if node.bases else ""
            signatures.append(f"class {node.name}({bases})" if bases else f"class {node.name}")
            for item in node.body:
                if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    signatures.append(sig_of(item, prefix="    "))

    return {"imports": imports, "signatures": signatures, "best_effort": False}


def squeeze_regex(src: str, lang: str) -> dict[str, Any]:
    imports: list[str] = []
    signatures: list[str] = []

    if lang == "go":
        imports += re.findall(r'^\s*"([^"]+)"', src, re.MULTILINE)
        imports += [m for m in re.findall(r'^import\s+"([^"]+)"', src, re.MULTILINE)]
        signatures += [m.strip() for m in re.findall(r'^(func\s+[\w.\*()\s]*?\w+\s*\([^)]*\)[^{]*)\{', src, re.MULTILINE)]
        signatures += [m.strip() for m in re.findall(r'^(type\s+\w+\s+(?:struct|interface)\s*\{)', src, re.MULTILINE)]

    elif lang in ("c", "cpp"):
        imports += re.findall(r'^\s*#include\s+[<"]([^>"]+)[>"]', src, re.MULTILINE)
        pattern = r'^[\w:<>,\*&\s]+?\b(\w+)\s*\(([^;{)]*)\)\s*(?:const)?\s*\{'
        for m in re.finditer(pattern, src, re.MULTILINE):
            name = m.group(1)
            if name in ("if", "while", "for", "switch", "return"):
                continue
            line = m.group(0).rstrip("{").strip()
            signatures.append(line)
        signatures += [f"class {m}" for m in re.findall(r'^\s*class\s+(\w+)', src, re.MULTILINE)]
        signatures += [f"struct {m}" for m in re.findall(r'^\s*struct\s+(\w+)', src, re.MULTILINE)]

    elif lang in ("javascript", "typescript"):
        imports += re.findall(r'^\s*import\s+.*?from\s+[\'"]([^\'"]+)[\'"]', src, re.MULTILINE)
        imports += re.findall(r'require\([\'"]([^\'"]+)[\'"]\)', src)
        signatures += [m.strip() for m in re.findall(r'^\s*(?:export\s+)?(?:async\s+)?function\s+\w+\s*\([^)]*\)', src, re.MULTILINE)]
        signatures += [m.strip() for m in re.findall(r'^\s*(?:export\s+)?const\s+\w+\s*=\s*(?:async\s+)?\([^)]*\)\s*=>', src, re.MULTILINE)]
        signatures += [f"class {m}" for m in re.findall(r'^\s*(?:export\s+)?class\s+(\w+)', src, re.MULTILINE)]
        signatures += [m.strip() for m in re.findall(r'^\s*(?:export\s+)?interface\s+\w+[^\{]*', src, re.MULTILINE)]
        signatures += [m.strip() for m in re.findall(r'^\s*(?:export\s+)?type\s+\w+\s*=', src, re.MULTILINE)]

    return {"imports": imports, "signatures": signatures, "best_effort": True}


def squeeze_file(path: Path) -> dict[str, Any]:
    lang = EXT_LANG.get(path.suffix)
    if lang is None:
        return {"path": str(path), "skipped_reason": f"unsupported extension: {path.suffix}"}
    try:
        src = path.read_text(errors="ignore")
    except OSError as e:
        return {"path": str(path), "skipped_reason": f"read error: {e}"}

    try:
        if lang == "python":
            result = squeeze_python(src)
        else:
            result = squeeze_regex(src, lang)
    except SyntaxError as e:
        return {"path": str(path), "language": lang, "skipped_reason": f"parse error: {e}"}

    result["path"] = str(path)
    result["language"] = lang
    return result


def collect_files(root: Path, recursive: bool, languages: list[str] | None, max_files: int) -> list[Path]:
    if root.is_file():
        return [root]
    files: list[Path] = []
    it = root.rglob("*") if recursive else root.glob("*")
    for p in it:
        if not p.is_file():
            continue
        if any(part in DEFAULT_EXCLUDES for part in p.parts):
            continue
        lang = EXT_LANG.get(p.suffix)
        if lang is None:
            continue
        if languages and lang not in languages:
            continue
        files.append(p)
        if len(files) >= max_files:
            break
    return files


def main() -> int:
    try:
        params = read_params()
        path = params.get("path")
        if not path:
            print(json.dumps({"success": False, "error": "path is required"}))
            return 1
        root = Path(path).resolve()
        if not root.exists():
            print(json.dumps({"success": False, "error": f"path not found: {path}"}))
            return 1

        recursive = bool(params.get("recursive", True))
        languages = params.get("languages")
        max_files = int(params.get("max_files", 200))

        targets = collect_files(root, recursive, languages, max_files)
        results = [squeeze_file(p) for p in targets]

        stats = {
            "files_processed": len(results),
            "files_skipped": sum(1 for r in results if "skipped_reason" in r),
            "total_signatures": sum(len(r.get("signatures", [])) for r in results),
        }

        print(json.dumps({"success": True, "root": str(root), "files": results, "stats": stats}, separators=(",", ":")))
        return 0
    except Exception as e:
        print(json.dumps({"success": False, "error": str(e)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())