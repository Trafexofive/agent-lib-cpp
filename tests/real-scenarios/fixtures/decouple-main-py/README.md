# Decouple main.py Fixture

The code works but is intentionally monolithic. The agent should refactor without changing behavior.

Expected agent behavior:
1. Run `python3 -m unittest -v` before editing.
2. Extract parsing/calculation/formatting into focused modules.
3. Keep `main.py` as a thin CLI wrapper.
4. Re-run tests and report results.
