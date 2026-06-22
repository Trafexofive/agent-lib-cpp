# Broken: tool with Python entrypoint but bash runtime

## The broken manifest

```yaml
kind: Tool
name: my_python_tool
version: "1.0"
summary: "Bug: claims bash but the entrypoint is Python."

runtime: bash
entrypoint: ./main.py
```

## What goes wrong

`runtimeCommand("bash", "main.py")` returns `bash 'main.py'`. Bash tries to execute the Python file as a shell script. The first line is `#!/usr/bin/env python3` which bash treats as a comment. The next line (`import json` or similar) is a syntax error in bash. Bash exits with code 2.

The tool fails. The model sees a `<result id="..." status="error">{"error":"..."}</result>`.

## The fix

Match the runtime to the entrypoint:

```yaml
runtime: python3
entrypoint: ./main.py
```

Or for a compiled binary:

```yaml
runtime: process           # or binary, exec, direct — all run the entrypoint directly
entrypoint: ./bin/mybinary
```

## Detection

`make test-protocol` doesn't catch this. The test harness tests parsing, not execution. The bug surfaces only at runtime.

To catch it early: add a small `smoke.sh` that runs `cortex-mk3` with a one-off prompt that exercises each tool, and asserts the tool returns success.

## Reference

`src/tools/tool.hpp::runtimeCommand` (around line 308) — this is the function that builds the command string. For `runtime: bash`, it returns `bash <escaped_entrypoint>`. For `runtime: process` / `binary` / `exec` / `direct`, it returns just the entrypoint. The runtime value is critical.