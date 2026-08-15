#!/usr/bin/env python3
"""ipython — read-only Python analysis in a restricted namespace.

The discovery agent's only execution surface. No `open`, no `os`, no
`subprocess`, no `__import__`, no network. numpy + stdlib data modules are
pre-imported so retrieved JSON/tabular/text results can be crunched locally.
"""

from __future__ import annotations

import ast
import contextlib
import io
import os
import signal
import sys
from typing import Any

# Bootstrap the shared support library. `os` is used only for this path
# resolution — it is NOT exposed in the restricted execution namespace below.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(_HERE, "..", "..", "_shared", "src")))

from common import read_params, emit, fail  # noqa: E402

import json  # noqa: E402
import math  # noqa: E402
import re  # noqa: E402
import statistics  # noqa: E402
import collections  # noqa: E402
import itertools  # noqa: E402
import datetime  # noqa: E402
import numpy as np  # noqa: E402


# ── Restricted execution environment ──────────────────────────────────────
# Whitelisted builtins: everything dangerous is absent, so a `__builtins__`
# injection only ever exposes safe operations.
_SAFE_BUILTINS: dict[str, Any] = {
    name: getattr(__builtins__, name)
    for name in (
        "abs", "all", "any", "ascii", "bin", "bool", "bytearray", "bytes",
        "callable", "chr", "complex", "dict", "divmod", "enumerate", "filter",
        "float", "format", "frozenset", "getattr", "hasattr", "hash", "hex",
        "int", "isinstance", "issubclass", "iter", "len", "list", "map",
        "max", "min", "next", "oct", "ord", "pow", "print", "range", "repr",
        "reversed", "round", "set", "slice", "sorted", "str", "sum", "tuple",
        "type", "zip",
    )
}

# numpy lazily imports submodules at runtime via the builtin `__import__`;
# stripping it outright breaks `np.ndarray.mean` and friends. We keep a
# restricted `__import__` that only resolves whitelisted roots, so numpy still
# works while `os`/`sys`/`subprocess`/`pathlib`/etc. stay unreachable.
_real_import = __import__

_SAFE_MODULES = frozenset({
    "numpy", "json", "math", "statistics", "re", "collections", "itertools",
    "datetime", "functools", "operator", "decimal", "fractions", "random",
    "string", "typing", "numbers", "array", "bisect", "heapq", "calendar",
    "textwrap", "unicodedata", "warnings", "hashlib", "base64", "struct",
    "copy",
})


def _safe_import(name, globals_=None, locals_=None, fromlist=(), level=0):  # noqa: ANN001
    root = name.split(".", 1)[0]
    if root not in _SAFE_MODULES:
        raise ImportError(f"read-only ipython: import of '{name}' is not allowed")
    return _real_import(name, globals_, locals_, fromlist, level)

_SAFE_BUILTINS["__import__"] = _safe_import


def _timeout_handler(signum, frame):  # noqa: ANN001
    raise TimeoutError("ipython snippet exceeded its time budget")


def main() -> int:
    params = read_params()
    code = params.get("code") or ""
    if not code.strip():
        fail("code is required")

    try:
        timeout_sec = int(params.get("timeout_sec", 10))
    except (TypeError, ValueError):
        timeout_sec = 10
    timeout_sec = max(1, min(timeout_sec, 60))

    namespace: dict[str, Any] = {
        "__builtins__": _SAFE_BUILTINS,
        "np": np,
        "json": json,
        "math": math,
        "statistics": statistics,
        "re": re,
        "collections": collections,
        "itertools": itertools,
        "datetime": datetime,
    }

    signal.signal(signal.SIGALRM, _timeout_handler)
    signal.alarm(timeout_sec)
    out_buf = io.StringIO()
    result: str | None = None
    error: str | None = None
    try:
        with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(out_buf):
            # Trailing expression -> eval for a REPL-like result value.
            try:
                expr = ast.parse(code.strip(), mode="eval")
                value = eval(compile(expr, "<ipython>", "eval"), namespace, namespace)
                result = repr(value)
            except SyntaxError:
                exec(compile(code, "<ipython>", "exec"), namespace, namespace)
    except TimeoutError as e:
        error = str(e)
    except Exception as e:  # noqa: BLE001
        error = f"{type(e).__name__}: {e}"
    finally:
        signal.alarm(0)

    out = {"success": error is None, "output": out_buf.getvalue().rstrip()}
    if result is not None:
        out["result"] = result
    if error:
        out["error"] = error
    emit(out, 0 if error is None else 1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
