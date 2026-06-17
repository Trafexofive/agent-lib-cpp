---
description: Safe command execution. Run, capture, handle exit codes, timeouts, retryable errors.
argument-hint: "<command>"
---
## What this is
Run a shell command safely. Capture output. Handle errors. Retry intelligently.

## Command
$@

## Execution protocol

### Step 1: Understand what you're running
Before executing, answer:
- Is this destructive? (rm, mv, git push --force, drops, deletes) → gate with ask_cards(type_confirm)
- Is this read-only? (ls, cat, grep, find, git status, git diff) → safe, proceed
- Is this a build/test? (make, pytest, cargo test) → safe but may take time — set timeout
- Is this network-bound? (curl, wget, git clone) → may fail transiently — use retry_with_backoff

### Step 2: Execute
```
# Read-only, fast:
bash(command="$@")

# Long-running — set timeout:
bash(command="$@", timeout=120)

# Network/flaky — retry:
retry_with_backoff(command="$@", maxRetries=5, baseDelayMs=2000)

# Rate-limited provider — check first:
rate_limit_status → if throttled: delay or switch provider
```

### Step 3: Parse output
```
# Check exit code:
$? = 0  → success
$? != 0 → failure — parse stderr for the error type

# If output truncated (too large):
Check stderr for "output truncated" message → grep the output file for relevant lines
```

### Step 4: Handle failures

| Exit | Error pattern | Action |
|------|--------------|--------|
| 1 | "command not found" | Check if tool is installed. `which $TOOL` or `pacman -Q $TOOL`. |
| 1 | "permission denied" | Report. Don't auto-sudo. |
| 1 | "no such file" | Verify the path. Use read-safe to check. |
| 124 | timeout | Increase timeout or split the work. |
| 137 | OOM killed | Reduce scope. |
| 429/rate-limit | "too many requests" | retry_with_backoff(maxRetries=$N, baseDelayMs=5000) |
| Any | "connection refused" | Service not running. relic_status to check. |

### Step 5: Report
- Exit code + 1-line summary.
- If failed: what happened + fallback attempted.
- If succeeded: key output (truncated to relevant lines — don't dump everything).

## Anti-patterns
1. **DO NOT run destructive commands without ask_cards(type_confirm).** rm, mv, force-push, drop, delete → gate it.
2. **DO NOT ignore exit codes.** Non-zero = something went wrong. Investigate, don't shrug.
3. **DO NOT retry without backoff.** 5 rapid retries = rate-limit harder. Exponential backoff.
4. **DO NOT dump full output into chat.** Parse it. Extract what matters. artifact if it's substantial.
5. **DO NOT sudo without asking.** Permission denied? Report it. Let the user decide.
