---
name: test-driven
description: >
  TDD discipline: write test first, see it fail, implement, see it pass, refactor.
  Red-green-refactor cycle. Test behavior, not implementation. Load for sessions
  where test-first development is expected.
---

# Test-Driven Development Skill

## The cycle (red → green → refactor)

```
1. RED:    Write a failing test that defines the behavior you want
2. GREEN:  Write the minimum code to make the test pass
3. REFACTOR: Improve the code while tests stay green
4. REPEAT
```

## Phase 1: RED — Write the test first

```
# Before any implementation code exists:
read (understand the interface/contract) →
write (new test file) →
bash (run test — it MUST fail)
```

Rules:
- Test one behavior at a time. "User can log in with valid credentials."
- Name the test: `test_<scenario>_<input>_<expected>`
- If the test passes before you wrote code → the test is wrong or the behavior already exists.
- If you can't write a test because the interface doesn't exist → write the interface skeleton first (just the function signature), then the test.

## Phase 2: GREEN — Minimum implementation

```
edit (minimum code to pass the test) →
bash (run test — it MUST pass) →
verify-chain (lint, compile)
```

Rules:
- Write the SIMPLEST code that passes. Not the best code. Not the final code. The simplest.
- Don't add error handling, validation, or edge cases yet. Those are future tests.
- If the implementation is >20 lines for a single test → you're over-implementing.

## Phase 3: REFACTOR — Improve without breaking

```
edit (structural improvement) →
bash (ALL tests must still pass) →
verify-chain
```

Rules:
- Only refactor when all tests are green.
- Extract duplication, improve names, simplify logic.
- If any test fails during refactor → revert immediately.
- One refactoring step at a time.

## Test categories (in priority order)

1. **Happy path first:** The normal case. "Given X, I get Y."
2. **Edge cases:** Boundaries, empty, zero, null, max.
3. **Error conditions:** Invalid input, missing deps, failures.
4. **Regression:** The exact scenario that caused the bug being fixed.

## Test quality

| Good test | Bad test |
|-----------|----------|
| Tests behavior ("user gets a session token") | Tests implementation ("function calls generateToken()") |
| Tests one thing | Tests everything at once |
| Has descriptive name | Named `test_1` or `test_handler` |
| Fails for exactly one reason | Fails for 15 possible reasons |
| Uses the public API | Pokes at internal state |

## Anti-patterns
1. **DO NOT write code before the test.** Test first. Always.
2. **DO NOT write more than one test at a time.** One test, one implementation, one refactor. Loop.
3. **DO NOT skip the refactor phase.** Red-green-refactor. All three. Every cycle.
4. **DO NOT over-implement.** The minimum code to pass. Future tests drive future implementation.
5. **DO NOT test implementation details.** Test what the code DOES, not HOW it does it.
