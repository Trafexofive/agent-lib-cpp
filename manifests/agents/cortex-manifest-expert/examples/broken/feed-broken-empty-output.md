# Broken: feed with empty output, no `allow_empty`

## The broken manifest

```yaml
kind: Feed
name: silent_feed
version: "1.0"
summary: "Bug: poll script returns no stdout, no allow_empty opt-in."

runtime: bash
entrypoint: ./silent.sh
```

Where `silent.sh`:

```bash
#!/usr/bin/env bash
exit 0
# (no echo, no output)
```

## What goes wrong

After slice 7, this is a **load failure**:

```
[manifest] feed path not found: ... (or similar)
```

OR if the feed loads, the model has no `<silent_feed>` block in its prompt.

## Why

Slice 7 changed the default: empty stdout is a hard load failure. The error message hints at the fix:

> `feed script returned empty output (set allow_empty: true to allow)`

## The fix

Either:

1. Make the script actually output something:

```bash
#!/usr/bin/env bash
echo '{"ok": true, "note": "no data to report"}'
```

2. Or opt into empty output (only if intentional):

```yaml
allow_empty: true
```

## Detection

If a feed manifest parses but the feed isn't visible in `<action_available>` or `<feeds>`, the load failed. Check the agent's startup log for the load error.

## Tests that catch this

`src/testing/feed_manifest_test.hpp::testFeedEmptyOutputFailsWithoutAllowEmpty` — verifies the strict/permissive behavior.