---
description: Tool priority matrix — per-task tool chains. Inject for task-type awareness.
---
## Tool Priority by Task Type

Use this matrix to pick the right tool chain for the job. Tools are ordered — use them in this sequence.

### 🔍 Researching / exploring
```
autonomous_discover (auto-pin by keyword) → ethereal_read (inspect hits) → 
squeezer (map module interface) → artifact_create (persist findings)
# OR for web research:
spawn_and_collect (free model, web_search) → artifact_read → synthesize
```
Priority: **minimize context.** ethereal_read, not retain. Artifact, not chat.
Key synergy: autonomous_discover replaces manual grep for unknown codebases. spawn_and_collect is the one-shot research pattern.

### ✏️ Editing code
```
read (understand pattern) → edit (change) → bash (lint/build/test)
# If bash fails:
retry_with_backoff (flaky builds/tests) → rate_limit_status (check if throttled)
```
Priority: **verify after every edit.** Never chain edits without verification.
Key synergy: retry_with_backoff handles transient failures automatically.

### 🐛 Debugging
```
bash (reproduce) → autonomous_discover (find related code) → ethereal_read (inspect) →
bash (add diagnostics) → edit (fix) → bash (verify fix)
# For flaky failures:
retry_with_backoff (repro attempts) — confirms it's not transient
```
Priority: **one hypothesis at a time.** Reproduce before tracing.
Key synergy: autonomous_discover finds ALL related files, not just the one you thought of.

### 🔄 Refactoring
```
bash (run tests) → read/squeezer (understand) → edit (one change) →
bash (run tests — must match baseline) → bash (git diff --stat)
```
Priority: **behavior preservation.** Tests must pass before AND after.

### 🛡️ Security audit
```
autonomous_discover (keyword: auth, token, secret, exec, sql) → 
ethereal_read (trace data flow per hit) → artifact_create (findings table)
```
Priority: **follow the data.** Input → validation → execution. Whole chain.
Key synergy: autonomous_discover does a smarter, broader sweep than grep alone — it finds related files by concept, not just pattern.

### 🚀 Spawning sub-agents
```
# Single task (preferred):
spawn_and_collect (free model, one-shot) → artifact_read (result)

# Parallel tasks:
spawn_agent × N (free models) → harvest_completed → aggregate into artifact

# From pre-built definitions:
spawn_agent_def (def="researcher", prompt="...") — uses stored role prompt

# Context transfer between agents:
agent_clone_context (source agent → target agent) — pass pinned files

# Check status:
check_agent_status — list all running/tracked agents
rate_limit_status — check if provider is throttled before spawning
```
Priority: **delegate, don't bloat.** Parent orchestrates, doesn't do heavy lifting.
Key synergy: spawn_and_collect for single tasks, harvest_completed for parallel batches, agent_clone_context for state transfer, rate_limit_status for provider-aware routing.

### 🧠 Memory operations
```
memory_query (check existing) → memory_add_node (new fact) → 
memory_add_edge (connect) → memory_query (verify)
```
Priority: **query before create.** No duplicates. No session trivia.

### 📦 Artifact operations
```
artifact_list / artifact_search (discover existing) → artifact_create (draft) → 
artifact_edit (refine sections) → artifact_link (connect) → 
artifact_export_graph (visualize relationships) → artifact_read (verify)
# Cleanup:
artifact_delete (stale/wrong artifacts)
# Bulk:
artifact_update(mode="append") (add to end) — only for additive changes
artifact_update(mode="branch") (fork) — experimental copies
```
Priority: **search before create, edit don't rewrite, link for traceability.**
Key synergy: artifact_export_graph at the end of a multi-phase task shows the full deliverable chain as Mermaid or JSON.
