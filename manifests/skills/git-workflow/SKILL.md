---
name: git-workflow
description: >
  Safe git operations discipline. No blind staging, no force-push, no auto-delete.
  Teaches the agent how to stage, commit, branch, rebase, and clean up safely.
  Load for any session where git operations are expected.
---

# Git Workflow Skill

## Core rules (non-negotiable)

1. **Never `git add -A` or `git add .`** — stage specific files only.
2. **Never `git push --force`** — not even with confirmation. Report what needs force-push and why.
3. **Never delete branches without ask_cards approval.**
4. **Never commit without reviewing `git diff --stat` and `git diff --name-status` first.**
5. **Never rebase a dirty working tree.** Commit or stash first.
6. **Never rebase shared branches** (main, master, release/*).

## Staging protocol

```
# Before staging:
git diff --stat          # What changed? Any unexpected files?
git diff --name-status   # Added, modified, deleted? Any files you didn't touch?

# Stage:
git add <specific-file> <specific-file>   # NEVER -A or .

# Verify staging:
git diff --cached --stat  # What's about to be committed?
```

## Commit protocol

```
# Message format:
<type>: <short description>

<optional body — why, not what>

# Types:
fix:      Bug fix
feat:     New feature
refactor: Structural change, no behavior change
test:     Tests only
docs:     Documentation only
chore:    Build, config, deps
perf:     Performance improvement
revert:   Reverting a previous commit
```

## Branch protocol

```
# Create branch:
git checkout -b <type>/<description>
# Examples: fix/auth-token-expiry, feat/user-export, refactor/db-pool

# Before switching:
git status --short   # Clean? If not, stash or commit.

# Rebase on main:
git fetch origin
git rebase origin/main
# Conflict? Stop. Report conflicting files. Let user resolve.
```

## Cleanup protocol (use /git-sweep for full sweep)

```
# Find merged branches:
git branch --merged main | grep -v "main\|master\|release"

# Delete local (safe — won't delete unmerged):
git branch -d <branch>

# Delete remote (ask first):
git push origin --delete <branch>
```

## Anti-patterns

| Don't | Do |
|-------|-----|
| `git add -A` | `git add src/auth.c tests/test_auth.c` |
| `git push --force` | Explain why force-push is needed, let user do it |
| `git commit -m "stuff"` | Proper type + description |
| Commit with failing tests | Run verify-chain first |
| Rebase without fetching | `git fetch` before `git rebase` |
| Delete branches silently | ask_cards(multi_choice) for branch deletion |
