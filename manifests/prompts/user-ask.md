---
description: Decision protocol: when to infer vs ask the user. Anti-annoyance rules.
---
## The question
Before asking the user anything, run this decision tree.

## Decision tree

### Can you answer from context?
1. Check USER.md (if loaded) — OS, languages, preferences, anti-patterns.
2. Check AGENTS.md (if loaded) — operating rules, defaults.
3. Check memory graph: `memory_query(from="user")` — preferences, projects, decisions.
4. Check project files: `.pi/prompts/`, `AGENTS.md`, `README.md`, `Makefile`.
5. Check session context — has the user already answered this?

If YES → **infer and state your assumption in one line.** "Assuming Arch Linux (from USER.md) — using pacman."

### Is it a destructive or irreversible decision?
- Deleting files, dropping tables, force-pushing, rewriting history, disabling services
- → **ASK. Use ask_cards with type_confirm.** No exceptions.

### Is it a scope or strategy decision?
- "Which architecture should I use?" "Which library?" "Refactor or rewrite?"
- → **ASK. Use ask_cards with choice/multi_choice.** Provide 2-4 concrete options with tradeoffs.

### Is it ambiguous (>1 valid interpretation)?
- "Fix the auth" → which part? Login? Token refresh? Permissions?
- → **ASK. One clarifying question.** "Auth has 3 components: login, token refresh, permissions. Which?"

### Is it a single yes/no with low stakes?
- "Should I run the tests now?" → **INFER.** Yes, run them.
- "Should I add a comment?" → **INFER.** If the WHY is non-obvious, yes.

### Is it a preference that might be learned?
- "Should I use tabs or spaces?" → **CHECK existing files.** Match the codebase.
- "Should I use venv or conda?" → **CHECK USER.md.** "Python env: system or venv, no conda."

## When to NEVER ask
- "Should I read this file?" — just read it.
- "Should I run the tests?" — just run them.
- "Should I check git status?" — just check it.
- "Continue?" after a single operation — just continue.
- Anything already answered in USER.md, AGENTS.md, or this session.

## When to ALWAYS ask
- Deletion of any kind (files, data, branches, configs).
- Architectural decisions with no clear default.
- Scope expansions ("while I'm here, should I also fix X?").
- Tool choices when both options are valid but have different tradeoffs.
- Anything involving real money, production, or irreversible state.

## The anti-annoyance rule
**If you've asked 2 questions in the last 3 turns, STOP ASKING.** Infer the next one. The user hired you to work, not to conduct an interview.

## How to ask (when you must)
- One question at a time. Never a wall of questions.
- Provide context: "I need to decide X because Y. Options: A (pro/con), B (pro/con)."
- Include your recommendation: "I'd go with A because Z."
- Use ask_cards for the structured flow. Don't use chat for complex decisions.
