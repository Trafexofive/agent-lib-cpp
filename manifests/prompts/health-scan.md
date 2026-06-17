---
description: Codebase health report. Lint + type-check + test + audit. Aggregate scores per category.
argument-hint: "[target-directory]"
---
## What this is
A full health scan of the codebase. Run lint, type-check, tests, and security audit. Aggregate into a single scored report.

## Target
$@ (default: current directory)

## Protocol

### Phase 1: Discovery
```
autonomous_discover (keyword: lint config, test config, build system, security) →
grep for: .eslintrc, pyproject.toml, Makefile, Cargo.toml, go.mod, package.json
```
Identify: language(s), build system, test framework, lint tools available.

### Phase 2: Lint scan (spawn parallel)
```
spawn_agent(name="lint-scan", prompt="Run all available linters on $TARGET. For each: tool used, total issues, errors vs warnings, top 5 most common issues.")
```
Adapt per language: ruff/pylint/flake8 (Python), eslint (TS/JS), shellcheck (Bash), clang-format (C/C++).

### Phase 3: Type-check / compile scan
```
spawn_agent(name="type-scan", prompt="Run type-checker/compiler on $TARGET. Report: errors, warnings, files affected.")
```
Adapt: mypy (Python), tsc (TS), gcc -Wall (C), cargo check (Rust), go vet (Go).

### Phase 4: Test scan (use /batch-test)
```
Invoke /batch-test for $TARGET
```

### Phase 5: Security audit (use /audit)
```
spawn_agent(name="audit-scan", prompt="Run security audit on $TARGET. Focus: auth, injection, secrets, unsafe operations.")
```

### Phase 6: Aggregate
```
health_report = {
  target: $TARGET,
  language: [...],
  timestamp: now,

  lint: {
    tool, total_issues, errors, warnings, score: A/B/C/D/F
  },
  types: {
    tool, errors, warnings, score: A/B/C/D/F
  },
  tests: {
    total, passed, failed, skipped, flaky, score: A/B/C/D/F
  },
  security: {
    critical, high, medium, low, score: A/B/C/D/F
  },

  overall_score: weighted_average,
  critical_blockers: [...issues that MUST be fixed],
  top_5_issues: [...most impactful fixes]
}
```

Scoring:
- A: 0 issues
- B: <5 minor issues
- C: <10 issues, some moderate
- D: >10 issues or 1+ high severity
- F: Critical security issues or build is broken

### Phase 7: Report
```
artifact_create(name="health-report-$TARGET", type="document", content=health_report)
agent_status_log(type="complete"): "Health: $SCORE. N critical blockers. Report: $ARTIFACT_ID"
```

## Anti-patterns
1. **DO NOT run everything sequentially.** Lint, type-check, test, and audit happen in parallel.
2. **DO NOT skip security for "small projects."** Small projects have the worst security.
3. **DO NOT report without scores.** "Some lint issues" → useless. "Lint: C (8 issues, 3 errors)" → useful.
