---
description: Create a new prompt template. Interactive wizard or skeleton drop.
argument-hint: "[prompt-name] [purpose]"
---
You are drafting a prompt template for Pi's prompt system (`~/.pi/agent/prompts/`).

## What you're building

A `.md` file that becomes a `/command`. When a user types `/command`, it expands into a context block that teaches the agent what to do, how to think, and which tools to chain.

A good prompt template is NOT a role description. It's an **operational protocol** — a dense, actionable instruction set that alters agent behavior for a specific task.

## Anatomy of a powerful prompt

### Frontmatter (required)
```yaml
---
description: One-line summary of what this prompt does
argument-hint: "<required-arg> [optional-arg]"
---
```

### Body structure (pick what applies)
1. **Identity/role** — What the agent IS during this task (1-2 lines, no fluff)
2. **Objective** — What success looks like. Concrete, measurable.
3. **Tool protocol** — WHICH tools to use, in WHAT order, and WHY. This is the synergy layer.
4. **Anti-patterns** — "Here's how this task commonly fails. Don't do this."
5. **Output format** — What the agent produces. artifact? chat? edit? bash output?
6. **Halt condition** — When exactly is the agent DONE.

### Tool protocol — the most important section

Don't just list tools. Teach composition. Example:

```
## Tool protocol

1. grep/rg for the key pattern first — never read blindly
2. ethereal_read the 2-3 best hits (not read_and_retain — this is transient research)
3. IF more than 5 files matched → spawn a free sub-agent OR narrow your grep
4. Synthesize findings into an artifact (artifact_create) before responding
5. Report: artifact ID + 1-line summary — not the full content in chat
```

### Anti-patterns — scars, not preferences

Every prompt should encode at least 2 failure modes:

```
## Anti-patterns
- DO NOT read every file that matches. Pattern-search first, read the hits.
- DO NOT dump raw grep output into chat. Synthesize into structured findings.
- DO NOT retain all files you touch. ethereal_read for transient research.
```

## Your task

$@

## If $1 is empty — interactive mode

Ask these questions one at a time. Don't rush.

1. **What's the command name?** (becomes filename, e.g. `research`, `audit`, `debug-trace`)
2. **What's the one-line description?** (goes in frontmatter)
3. **What trigger scenario?** When would someone type this command? Be specific.
4. **What should the agent DO differently after this prompt is injected?** 
5. **What tools must it use, and in what composition?** (the synergy chain)
6. **What are the top 2 failure modes for this task?** (the anti-patterns)
7. **What's the halt condition?** (artifact created? bash exit 0? edit verified?)
8. **Any arguments?** (template vars like `$1`, `$2` for dynamic injection)

Then write the prompt template to `~/.pi/agent/prompts/$1.md`.

## If $1 is provided — skeleton mode

Write a complete prompt template with:
- Strong frontmatter (description + argument-hint)
- Identity block
- Objective block  
- Tool protocol section (with specific tool chains)
- Anti-patterns section (2+ failure modes)
- Output format
- Halt condition

Use `$@` as the task description body. Fill in everything else based on the command name and purpose.

Write it to `~/.pi/agent/prompts/$1.md`. NO placeholders, NO TODOs, NO "fill this in later."
