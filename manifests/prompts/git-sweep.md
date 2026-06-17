---
description: Branch cleanup and rebase. Find stale/merged branches, safe-delete, rebase current. No force-push.
argument-hint: "[base-branch: main]"
---
## What this is
Git housekeeping. Clean up merged branches, rebase your work on latest, report what was done.

## Base branch
$@ (default: `main` or `master` — detect from `git branch -a`)

## Protocol

### Phase 0: Safety check
```
git status --short     # Working tree MUST be clean. If dirty: STOP, tell user to commit/stash.
git branch --show-current  # Know what branch you're on.
```
Dirty working tree → abort. "Uncommitted changes. Commit or stash before running git-sweep."

### Phase 1: Fetch and scan
```
git fetch --prune       # Update remote refs, remove deleted remote branches
git branch -a           # List all branches (local + remote)
```

### Phase 2: Find merged branches
```
git branch --merged $BASE   # Local branches merged into base
git branch -r --merged $BASE  # Remote branches merged into base
```
Exclude: $BASE itself, any release/protected branches (`main`, `master`, `release/*`, `production`).

### Phase 3: Report — ask before deleting
```
ask_cards(multi_choice: "These branches are merged into $BASE. Delete?",
  options=[each merged branch with last commit date])
```
**Never auto-delete.** Always ask.

### Phase 4: Delete approved branches
```
git branch -d $BRANCH          # Local (safe — won't delete unmerged)
git push origin --delete $BRANCH  # Remote (if user approved)
```
If `-d` fails (unmerged) → switch to `-D` only with user confirmation.

### Phase 5: Find stale branches
```
# Branches with no commits in 30+ days, not merged:
for branch in $(git branch --no-merged $BASE | sed 's/^* //'); do
  git log -1 --format="%cr" $branch
done
```
Report stale branches. Don't delete without explicit approval.

### Phase 6: Rebase current branch
```
git rebase $BASE           # Rebase current work on latest base
```
If rebase conflict → STOP. Report conflicting files. Let user resolve.
If rebase succeeds → `git diff --stat $BASE..HEAD` to show what's new.

### Phase 7: Report
```
artifact_create(name="git-sweep-report", content={
  base_branch: $BASE,
  current_branch: $CURRENT,
  deleted_local: [...],
  deleted_remote: [...],
  stale_reported: [...],
  rebased: true/false,
  conflicts: [...]
})
```

## Safety rules
- **NEVER `git push --force`.** Not even with confirmation. Report what needs force-push and why.
- **NEVER `git add -A` or `git add .`** Stage specific files.
- **NEVER delete branches without ask_cards approval.**
- **NEVER rebase if working tree is dirty.**
- **NEVER rebase a shared branch (main, master, release/*).**

## Anti-patterns
1. **DO NOT auto-delete.** Every branch deletion is a user decision.
2. **DO NOT force-push.** If a rebase requires force-push, explain why and let the user do it.
3. **DO NOT skip the fetch.** You'll delete branches that were merged remotely but your local is stale.
