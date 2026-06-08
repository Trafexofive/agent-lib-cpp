# Decoupler Agent

**Type:** Specialist agent — problem decomposition and delegation  
**Status:** 🧪 staging

## Purpose
Takes complex, ambiguous tasks and breaks them into well-defined sub-tasks. Delegates each sub-task to the appropriate specialist sub-agent. Validates the decomposition plan before execution.

## One Sentence
"You give me a fuzzy problem — I give you a clean execution plan and dispatch it."

## Tools
| Tool | Purpose |
|------|---------|
| scaffold | Core decomposition tool — analyzes input, extracts sub-tasks, builds execution DAG, validates structure |
| exec | Run decomposition validation commands |
| list | Explore project structure for context |

## Feeds
| Feed | Purpose |
|------|---------|
| plan-progress | Tracks decomposition plan state — what's pending, active, done, blocked |

## Sub-Agents
| Agent | Purpose |
|-------|---------|
| reviewer | Validates decomposition plans — checks completeness, dependencies, naming |

## Edge Cases
- Single-task input → returns trivial 1-step plan (no over-decomposition)
- Circular dependencies → validator rejects, returns error to user
- Unknown domain → falls back to generic task breakdown patterns from context/

## Expected Input
```
"Build a user authentication system for a web app"
"Refactor the database layer to support sharding"
"Add real-time notifications to the messaging service"
```

## Expected Output
```
Plan: 4 sub-tasks
  1. [build] Scaffold auth module structure
  2. [review] Validate module layout
  3. [build] Implement password hashing
  4. [review] Review auth flow security
```

## Promotion Checklist
- [ ] Handles 10+ diverse decomposition requests correctly
- [ ] Sub-agent delegation works end-to-end
- [ ] Plan-progress feed reports accurate state
- [ ] Move to `manifests/agents/decoupler/`
