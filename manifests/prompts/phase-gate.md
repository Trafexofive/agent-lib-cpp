---
description: Pipeline phase awareness. Tool priorities shift per phase. Inject at pipeline transitions.
argument-hint: "<phase: plan|research|build|review|test|done>"
---
## Phase management — use the pipeline tools

```
pipeline_status              # Check current phase
pipeline_advance(reason="...")  # Advance to next phase
```
- Always check pipeline_status at the start of a major task.
- Advance only when the current phase's deliverable is complete.
- pipeline_advance requires a reason — state what's done and why the gate is met.

## Current phase: $1

Tool priorities shift per phase. Here's what matters NOW.

### PLAN phase
- **Primary tools:** autonomous_discover (map terrain fast), read, squeezer, grep
- **Goal:** scope, estimate, identify risks, produce step plan
- **Don't:** edit code, run tests, spawn builders
- **Deliverable:** plan artifact (checklist or document)

### RESEARCH phase
- **Primary tools:** autonomous_discover (auto-pin relevant files), grep, ethereal_read, spawn_and_collect (free, web_search)
- **Goal:** understand existing code, find patterns, identify constraints
- **Don't:** edit code, propose solutions
- **Deliverable:** research notes artifact

### BUILD phase
- **Primary tools:** read (match pattern), edit, bash (lint/build), retry_with_backoff (flaky builds)
- **Goal:** implement the plan. One step at a time. Verify each step.
- **Don't:** research unrelated code, refactor adjacent modules, plan next phase
- **Deliverable:** working code + passing tests

### REVIEW phase
- **Primary tools:** read, git diff, bash (tests), artifact_create (findings)
- **Goal:** correctness, security, maintainability. Severity-rated findings.
- **Don't:** edit code (unless fixing a critical bug), style-nitpick
- **Deliverable:** review artifact

### TEST phase
- **Primary tools:** read (test patterns), write (new tests), bash (run suite), retry_with_backoff (flaky tests)
- **Goal:** behavioral coverage. Happy path + edge + error + regression.
- **Don't:** test implementation details, write integration tests when unit would do
- **Deliverable:** test results artifact

### DONE phase
- **Primary tools:** artifact_export_graph (deliverable chain), artifact_link, context_status (release retained)
- **Goal:** final summary, link artifacts, release retained files
- **Don't:** start new work, optimize "one more thing"
- **Deliverable:** completion summary + artifact relationship graph

## Phase transition checklist
- [ ] Previous phase deliverable created as artifact
- [ ] Artifacts linked (plan → research → build → review)
- [ ] Irrelevant retained files released (context_status)
- [ ] pipeline_advance(reason="...") called with clear gate justification
- [ ] agent_status_log with phase transition

## Cross-phase tool: ask_cards
Use ask_cards at phase boundaries when you need structured user input:
- Plan → Build: "Here's the plan. Approve? Anything to change?"
- Build → Review: "Implementation complete. Ready for review?"
- Any destructive operation: type_confirm gate before deletion/rewrite.
