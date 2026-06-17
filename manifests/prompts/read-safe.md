---
description: Safe file reading protocol. Find, verify, read. Error handling for missing, binary, large files.
argument-hint: "<file-pattern-or-path>"
---
## What this is
A single atomic operation: find and read a file safely. Not research, not exploration — just "get me the contents of $1 without blowing up."

## Tool protocol (single path)

### Step 1: Resolve
```
# If given a path:
test -f $1 && echo "exists" || echo "missing"
# If given a pattern/name:
find . -name "$1" -type f 2>/dev/null | head -5
```

### Step 2: Check before reading
```
file $PATH              # Is it text? binary? symlink?
wc -l $PATH             # How big? >2000 lines needs chunking
ls -lh $PATH            # Size in human-readable
```

### Step 3: Read
```
# Text file, <2000 lines:
read(path="$PATH")

# Text file, >2000 lines — chunk it:
read(path="$PATH", offset=1, limit=2000)
read(path="$PATH", offset=2001, limit=2000)
# ... until done

# Binary file (png, jpg, etc.):
read(path="$PATH")  # Will render as image attachment

# Multiple matches found:
ethereal_read(paths=[...])  # Read all hits ephemerally
```

### Step 4: Decide retention
```
# One-turn use → ethereal_read (default)
# Multi-turn reference → read_and_retain
# Just checking contents → ethereal_read, cycles=1
```

## Error modes and fallbacks

| Error | Fallback |
|-------|----------|
| File not found | `find . -iname "*$1*"` — fuzzy search. Report: "not found, did you mean X?" |
| Permission denied | Report: "can't read $PATH (permission). Try sudo?" |
| Binary file | `file $PATH` confirms. Read anyway if image (pi renders it). Else: skip and report. |
| File too large (>10MB) | Report: "$PATH is X MB — too large to read fully. Use `head -500` or grep instead." |
| Symlink loop | Report and skip. |
| Directory (not file) | `ls $PATH` to list contents. Report: "that's a directory. Here's what's inside." |

## Anti-patterns
1. **DO NOT read without checking.** `file` and `wc -l` first. Always.
2. **DO NOT retain by default.** ethereal_read unless you'll need it for 3+ turns.
3. **DO NOT read entire 10MB log files.** grep for the relevant lines first.
4. **DO NOT guess the path.** Resolve it. If ambiguous, show options.

## Halt condition
File read successfully and contents available. If error, fallback attempted and reported.
