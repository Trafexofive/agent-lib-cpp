---
description: Parallel test runner. Find tests, spawn sub-agents, harvest, aggregate into single report.
argument-hint: "[test-target]"
---
## What this is
Run tests in parallel by spawning sub-agents. Aggregate results into one artifact. Faster than sequential.

## Target
$@ (if empty, find all tests automatically)

## Protocol

### Phase 1: Discover tests
```
# Find test files:
find . -name "*.test.*" -o -name "*_test.*" -o -name "test_*" -o -name "*spec.*" | head -20

# Find test commands (Makefile, package.json, etc.):
grep -r "^test:" Makefile CMakeLists.txt 2>/dev/null
grep '"test"' package.json 2>/dev/null
```
If `$1` is provided, scope to that target only.

### Phase 2: Partition
Group tests into batches (max 5 parallel agents). Each batch should be independent — no shared state.

```
Batch 1: unit tests (src/)
Batch 2: integration tests (tests/integration/)
Batch 3: end-to-end tests (tests/e2e/)
# OR by module:
Batch 1: auth tests
Batch 2: database tests
Batch 3: API tests
```

### Phase 3: Execute in parallel
```
# Spawn all batches simultaneously:
spawn_agent(name="test-batch-1", prompt="Run tests: <batch-1-command>. Report: passed, failed, skipped, output for each failure.", model="opencode/deepseek-v4-flash-free")
spawn_agent(name="test-batch-2", prompt="Run tests: <batch-2-command>. Report as above.", model="opencode/deepseek-v4-flash-free")
...
→ wait_for_agent for each
→ harvest_completed
```

Use `retry_with_backoff` within each sub-agent for flaky tests.

### Phase 4: Aggregate
```
artifact_create(name="test-results-$TARGET", type="output", content={
  total_passed: N,
  total_failed: N,
  total_skipped: N,
  batches: [
    { name, passed, failed, skipped, failures: [{test, error}] }
  ],
  flaky_tests: [{test, attempts}],
  overall: PASS | FAIL | FLAKY
})
```

### Phase 5: Report
- artifact ID + pass/fail/flaky summary.
- If failures: list failing tests with 1-line error.
- If flaky: flag tests that passed on retry.

## Anti-patterns
1. **DO NOT run all tests in one agent.** Parallel is the point.
2. **DO NOT mix test frameworks in one batch.** pytest + jest + cargo test → separate agents.
3. **DO NOT treat flaky tests as passing.** Report them separately.
4. **DO NOT skip discovery.** Find ALL tests, not just the ones you know about.
