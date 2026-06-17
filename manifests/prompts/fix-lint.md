---
description: Batch lint-and-fix loop. Find all lint errors, fix one by one, verify after each. Loop until clean.
argument-hint: "[target]"
---
## What this is
Iterative lint fixing. Not a one-shot "fix everything" — fix one issue, verify, repeat. Prevents cascading breakage.

## Target
$@ (file, directory, or glob)

## Protocol

### Phase 0: Baseline
```
# Run the linter. Record starting state.
<lint-command> $TARGET 2>&1 | tee /tmp/lint-baseline.txt
```
Count: total errors, total warnings. This is your starting score.

### Phase 1: Categorize
Group errors by type (not by file). Fix one category at a time — this prevents whack-a-mole.

```
Category 1: Syntax errors (fix first — everything else is noise if syntax is broken)
Category 2: Type errors
Category 3: Unused imports/variables
Category 4: Style/formatting
Category 5: Complexity warnings
```

### Phase 2: Fix loop
```
for each category:
  for each error in category:
    edit (fix the specific line) →
    verify-chain (lint → compile → test) →
    if verify passes: next error
    if verify fails: revert, note, skip to next
```
- Fix ONE error at a time. Not one file — one error.
- Verify after EVERY fix. verify-chain is non-negotiable.
- If a fix breaks something → revert immediately. Note the error, move on.

### Phase 3: Report
```
artifact_create(name="fix-lint-report", content={
  target: $TARGET,
  before: { errors: N, warnings: M },
  after: { errors: N, warnings: M },
  fixed: [...errors fixed],
  skipped: [...errors skipped with reason],
  broken_by_fix: [...errors where fix caused regression]
})
```

### Phase 4: Hard cases (what to do when you can't fix something)

| Situation | Action |
|-----------|--------|
| Fix breaks tests | Revert. Report with test failure details. |
| Error in generated code | Skip. "(generated — not fixing)" |
| Error requires refactoring >20 lines | Skip. Report: "requires refactor — run /refactorer first" |
| Disagree with the lint rule | Skip. "(lint rule arguably wrong — user decides)" |
| Fix touches 5+ files for one error | Skip. "(high blast radius — review manually)" |

## Anti-patterns
1. **DO NOT fix everything at once.** One error, one edit, one verify. Loop.
2. **DO NOT fix generated code.** If it's in `build/`, `dist/`, `node_modules/`, `.gen/` → skip.
3. **DO NOT disable lint rules to make errors go away.** Fix the code. Disabling is user's call.
4. **DO NOT continue if a fix breaks tests.** Revert, report, move to next error.
5. **DO NOT mix lint fixes with feature changes.** This is a cleanup pass, not a refactor.
