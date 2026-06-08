# TDD Python CLI Fixture

Intentional bug: `slugify()` keeps underscores. The tests define the desired behavior.

Expected agent behavior:
1. Run `python3 -m unittest -v` first and observe failure.
2. Make the minimum fix in `main.py`.
3. Re-run tests and report pass/fail.
