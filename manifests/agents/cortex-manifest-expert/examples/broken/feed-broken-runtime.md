# Broken: feed with mismatched runtime / entrypoint

## The broken manifest

```yaml
kind: Feed
name: python_pretending_to_bash
version: "1.0"
summary: "Bug: runtime says bash but entrypoint is Python."

runtime: bash
entrypoint: ./poll.py
```

Where `poll.py`:

```python
#!/usr/bin/env python3
import json
print(json.dumps({"ok": True}))
```

## What goes wrong

`runtimeCommand` builds `bash ./poll.py`. Bash tries to execute the Python file as a shell script and fails with a syntax error (or runs it line-by-line, executing the shebang as a comment and then choking on `import`).

The poll output is empty or an error string, and the load fails.

## The fix

Match the runtime to the entrypoint language:

```yaml
runtime: python3
entrypoint: ./poll.py
```

## Detection

`popen(cmd, "r")` will surface a non-zero exit; the feed manifest loader will report the error in its `error` field. Or run the script manually:

```bash
bash ./poll.py    # will fail
python3 ./poll.py # works
```

## Tests

`make test-feeds` runs all feed manifest tests. The Python example at `examples/feeds/example-python/feed.yml` shows the correct setup.