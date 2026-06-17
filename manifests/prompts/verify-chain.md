---
description: Post-edit verification protocol. Lint, type-check, test, git diff. Ordered. Per-language matrix.
argument-hint: "<edited-file>"
---
## What this is
After editing a file, verify the change didn't break anything. Language-aware. Ordered by speed (fast checks first, slow checks last).

## File edited
$@

## Verification chain (run in order — stop on critical failure)

### Step 1: Git diff (10ms)
```
git diff --stat            # What files changed? Did you touch more than intended?
git diff $@                # Show the actual change. Review it yourself before testing.
```
If diff touches files you didn't intend to change → STOP. Investigate.

### Step 2: Lint (fast, <5s)
| Language | Command |
|----------|---------|
| C/C++ | `clang-format --dry-run -Werror $@` |
| Python | `ruff check $@` or `flake8 $@` |
| Bash | `shellcheck $@` |
| TypeScript | `eslint $@` or `tsc --noEmit` |
| Go | `gofmt -d $@` and `go vet ./...` |
| Rust | `cargo fmt --check && cargo clippy` |
| Markdown | skip |

Lint warnings → fix inline. Lint errors → fix before proceeding.

### Step 3: Type-check / compile (medium, <30s)
| Language | Command |
|----------|---------|
| C | `gcc -Wall -Wextra -Werror -c $@` or `make` |
| C++ | `g++ -std=c++11 -Wall -Wextra -Werror -c $@` or `make` |
| Python | `mypy $@` (if configured) |
| TypeScript | `tsc --noEmit` |
| Go | `go build ./...` |
| Rust | `cargo check` |

Compile error → fix. Do not proceed to tests.

### Step 4: Tests (slow, may take minutes)
```
# Run the specific test for what you changed:
<test-runner> <specific-test>

# If that passes, run the full suite:
<test-runner>
```
Use `retry_with_backoff` for flaky tests before concluding failure.

Test failure → read the failing test. Is it a real regression? Fix it. Is the test wrong? Report, don't silently skip.

### Step 5: Runtime check (if applicable)
```
# Does the program start?
./program --help
# Does the API respond?
curl localhost:8080/health
```

## Verification matrix by change type

| Change type | Minimum verification |
|-------------|---------------------|
| Comment only | git diff (step 1) |
| Rename/reformat | lint (step 2) |
| Logic change | lint + compile + run affected tests (steps 2-4) |
| API/signature change | lint + compile + full test suite (steps 2-4) |
| Config change | parse/validate config + restart service |
| New file | lint + compile + verify it's importable + add test |

## Anti-patterns
1. **DO NOT skip verification.** "It's just a one-line change" → that one line broke prod.
2. **DO NOT run tests before lint.** 100 tests fail because of a syntax error → waste.
3. **DO NOT trust the old test suite blindly.** If all tests pass but your change should've broken something, the tests are incomplete.
4. **DO NOT chain edits without verifying.** Edit → edit → edit → "why is everything broken?" One edit, one verify.
