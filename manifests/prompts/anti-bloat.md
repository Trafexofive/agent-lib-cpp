---
description: Hard context rules. Inject when the agent is hoarding or bloating parent context.
---
## Context discipline — HARD RULES

### Read tools — when to use which

| Tool | Use case | Context cost |
|------|----------|--------------|
| grep/rg | Pattern search — find files, not read them | Near zero |
| ethereal_read | Read 1-3 files for CURRENT task. Auto-evicts. | Temporary |
| read | Read files you need for MULTIPLE turns | Permanent until released |
| read_and_retain | Architecture docs, config files, key interfaces ONLY | Permanent, injected every turn |
| squeezer | Map a module's interface — imports, exports, signatures | Low (bodies stripped) |

### Sub-agent rules

- **NEVER call web_search or web_search_recursive in the parent.** Spawn a free sub-agent.
- **NEVER call web_fetch_page in the parent if you need more than 1 page.** Spawn.
- **For >2 file edits, >3 files to read, or >1 web query → spawn a sub-agent.**
- Always wait_for_agent after spawn. Always harvest results.
- Free models for research and drafts. Paid models for complex reasoning only.

### Chat vs Artifact

- Research findings → artifact, not chat.
- Chat gets: artifact ID + 1-line summary.
- If it took >1 turn to produce → it's an artifact.

### Before reading a file, ask:
1. Do I know what I'm looking for? (grep first if not)
2. Is this a 1-turn read or permanent reference? (ethereal vs retain)
3. Can squeezer give me the signature map faster? (use squeezer for module exploration)

### Context checkpoints
- After every 3 file reads: run context_status. If >8 files retained → release the least relevant.
- After every major phase: check if sub-agent output can replace retained files.
