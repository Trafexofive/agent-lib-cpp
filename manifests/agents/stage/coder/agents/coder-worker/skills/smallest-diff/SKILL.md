---
name: smallest-diff
description: Minimal change that satisfies the task; no drive-by refactors.
---
# smallest-diff

- One logical change cluster at a time.
- Match local patterns; do not expand scope.
- Prefer fix-forward over rewrite.
- If already satisfied → correct no-op, no fake edit.
