# MK3 Prompt Library

Reusable persona/task prompts for bespoke agents. These are stdlib building blocks: copy, adapt, version, and promote only what survives real use.

## Current set (13 keepers)

### Core personas
- `builder.md` — implement features from spec
- `tester.md` — contract-driven testing (happy path + edge + error + regression), with optional parallel execution mode
- `debugger.md` — hypothesis-driven debugging: reproduce → isolate → root cause → minimal fix
- `researcher.md` — codebase research (sweep → map → drill → synthesize), plus web research mode via sub-agent delegation
- `refactorer.md` — smell-first structural refactoring: name the smell → one change → preserve behavior
- `reviewer.md` — correctness-first review with severity ratings (critical/high/medium), no style nits

### Task-specific
- `planner.md` — planning and decomposition
- `audit.md` — structured audit/inspection
- `git-sweep.md` — git hygiene and cleanup
- `health-scan.md` — quick health check
- `verify-chain.md` — verification discipline

### Recovery / safety net
- `fix-lint.md` — lint fix automation
- `error-fallback.md` — graceful error recovery

## What was removed (2026-06-17 consolidation)

Removed 25 files — pi-specific slop, duplicates, and unused experiments:

**Pi-specific config/session prompts** (belong in ~/.pi, not in agent-lib):
anti-bloat, artifact-flow, ask-cards, context-engineer, memory-discipline, new-session-prompt, phase-gate, read-safe, run-safe, stop-hoarding, stop-nitpicking, stop-overengineering, substrate-lab, tool-matrix, user-ask, user-preamble

**Unused experiments** (never promoted from seed):
convert-into-prompt, create-prompt, init-project

**Duplicates** (content merged into keepers then deleted):
- research → merged into researcher.md (web research mode)
- batch-test → merged into tester.md (parallel execution mode)
- refactor, review, write-test, debug-trace → no unique content beyond keepers

## Usage

Reference by filename in agent manifest `system` or `promptTemplate` fields.
