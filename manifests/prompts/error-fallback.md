---
description: Tool failure recovery. Per-tool fallback map. Retry → fallback → escalate.
---
## What this is
A tool just failed. Here's the recovery protocol — what to try next, in order.

## The fallback ladder (always try in this order)

### Level 1: Retry (transient failures)
```
retry_with_backoff(command="<same command>", maxRetries=3, baseDelayMs=2000)
```
Use for: network errors, rate limits, timeouts, connection refused.

### Level 2: Alternative tool (same outcome, different path)
| Failed tool | Fallback |
|-------------|----------|
| `web_search` | `spawn_and_collect` with free model doing web_search |
| `read` (large file) | `grep` for relevant lines + `read` with offset/limit |
| `bash` (command not found) | `which <tool>` or `pacman -Q <tool>` |
| `edit` (match not found) | `grep` for the exact text, re-read the file |
| `spawn_agent` (provider error) | `rate_limit_status` → switch provider |
| `artifact_create` (disk full) | `df -h ~/.pi/agent/artifacts*` |

### Level 3: Workaround (different approach)
- Can't read a file? → `find` alternatives, check symlinks, check permissions.
- Can't spawn sub-agent? → Do it in the parent (last resort — warn about context bloat).
- Can't edit a file? → `write` the full file (last resort — preserves nothing).
- Can't run a command? → Parse the error, suggest a fix, ask the user.

### Level 4: Escalate (ask the user)
If all fallbacks fail:
```
ask_cards(note: "Tool X failed after retries and fallbacks. Error: [details]. Options: [A, B, C].")
```
Do NOT silently give up. Report what failed, what you tried, what you need.

## Per-tool recovery map (quick reference)

### read
```
File not found → find fuzzy match → report options
Permission denied → report, don't sudo
Binary file → file $PATH → read as image or skip
Too large → grep relevant lines → read chunked
```

### edit
```
oldText not found → grep for substring → re-read file → verify exact match
Multiple matches → make oldText more specific (include surrounding context)
Edit rejected → check for special characters, escapes
```

### bash
```
Command not found → which/pacman check → suggest install
Permission denied → report, don't sudo
Timeout → increase timeout or split work
Non-zero exit → parse stderr, apply action from run-safe error table
```

### spawn_agent / spawn_and_collect
```
Provider error → rate_limit_status → switch free provider
Timeout → check artifact_search for partial output (sub-agent may have created artifacts)
Model not found → agent_def_catalog → pick available model
```

### web_search / web_fetch_page
```
Network error → retry_with_backoff, maxRetries=3
No results → refine query, try different keywords
Rate limited → search_cache_clear → wait → retry
```

### artifact_*
```
artifact_create fails → df -h check disk → artifact_edit existing instead
artifact_edit section not found → artifact_read verify section name → artifact_update append instead
```

## Anti-patterns
1. **DO NOT retry without backoff.** Exponential backoff. Max 5 retries.
2. **DO NOT silently switch approaches.** If you fall back from spawn_agent to doing it in parent, WARN about context bloat.
3. **DO NOT escalate prematurely.** At least try levels 1-3 before asking the user.
4. **DO NOT hide failures.** Report what failed, what you tried, and the current state.
