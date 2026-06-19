# Runtime templates: compiled tools and feeds

These templates show the lightweight compiled-runtime convention.

## Runtime contract

Use `runtime: process` (aliases: `binary`, `direct`, `exec`) for compiled/direct entrypoints. Add `build` so MK3 can produce the executable when it is missing.

### Tool

```yaml
kind: Tool
implementation:
  runtime: process
  entrypoint: ./build/my_tool
  build:
    command: make
    cwd: .
    output: ./build/my_tool
    auto: true
```

MK3 invokes Agent script tools as:

```text
./build/my_tool <input.json>
```

The tool should print one JSON object to stdout:

```json
{"success": true, "output": "..."}
```

### Feed

```yaml
kind: Feed
runtime: process
entrypoint: ./build/my_feed
build:
  command: make
  cwd: .
  output: ./build/my_feed
  auto: true
```

MK3 invokes feeds as:

```text
./build/my_feed
```

The feed should print JSON or plain text to stdout. JSON object keys become feed summary lines.

## Templates

- `cpp-tool/` — C++ tool using jsoncpp
- `c-tool/` — C tool, no external JSON dependency
- `cpp-feed/` — C++ feed using jsoncpp
- `c-feed/` — C feed, no external dependency

Each folder supports:

```bash
make
make smoke
```
