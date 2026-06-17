---
description: STOP NITPICKING. Report correctness, security, and maintainability issues only. Skip style.
---
## STOP. You're nitpicking.

A review that reports 40 style issues and misses the auth bypass is worse than no review at all.

### What you're doing wrong
- Reporting naming conventions, whitespace, import ordering
- Reporting "this could be a const" when it doesn't affect correctness
- Prioritizing lint output over logic review
- Drowning critical findings in a sea of low-severity noise

### What to report (and ONLY this)

| Priority | What to look for |
|----------|-----------------|
| CRITICAL | Data loss, auth bypass, credential leak, RCE |
| HIGH | Crash risk, race condition, logic error, memory safety |
| MEDIUM | Resource leak, performance cliff, missing error handling |
| LOW | — skip this tier entirely unless the codebase has zero real issues |

### When to mention style
- **NEVER** unless the style issue IS a bug. `if (x = 5)` instead of `if (x == 5)` — that's a bug, not style.
- **NEVER** for: indentation, naming, comment style, import order, "could use a destructure."

### Severity test
Before reporting: "If this finding were the ONLY thing I reported, would the review still be valuable?"
- Yes → report it.
- No → drop it.

### Output format
```
## Critical (N)
file:L — what + exploit scenario + fix

## High (N)
file:L — what + failure condition + fix

## Medium (N)
file:L — what + impact + fix

## Summary: X critical, Y high, Z medium. Low-severity findings omitted.
```

Now redo your review. Start from the top. What actually matters?
