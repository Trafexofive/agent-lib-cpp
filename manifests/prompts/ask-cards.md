---
description: Structured alignment engine. Use ask_cards for multi-step decisions, approvals, and user input.
---
## When to use ask_cards

ask_cards is a blocking dialog engine. Use it when you need structured user input — not when you can infer or default.

| Use ask_cards for | Don't use ask_cards for |
|---|---|
| 2+ clarification questions needed | Single yes/no (just ask in chat) |
| Destructive/irreversible actions | "What file should I read?" (just pick one) |
| Scope changes, strategy decisions | Technical facts you can verify with bash |
| Choosing between implementation approaches | "Should I use grep or rg?" (default: rg) |
| Approval gates (deploy, delete, rewrite) | Trivial confirmations |
| Architecture alignment | "Continue?" after a single read |

## Card types — when to use each

| Card | Use case | Example |
|------|----------|---------|
| `choice` | Pick one from N options | "Which database? Postgres / SQLite / Neo4j" |
| `multi_choice` | Pick zero or more | "Which services to deploy?" |
| `confirm` | Yes/no with optional delay | "Proceed with refactor?" |
| `type_confirm` | Type a word to confirm destruction | "Type DELETE to drop table" |
| `text` | Free-form short input | "Branch name?" |
| `textarea` | Multi-line input | "Paste the error log" |
| `secret` | Masked input (tokens, passwords) | "API key?" |
| `number` | Numeric input with min/max | "How many replicas? (1-10)" |
| `ranker` | Drag-to-reorder priorities | "Order these features by importance" |
| `chain` | Nested sub-flow | "Database config (host, port, name, ssl)" |
| `loop` | Repeat a card N times | "Add endpoints (one at a time)" |
| `note` / `info` | Display-only messages | "Starting deployment pipeline..." |
| `section_header` | Visual divider | "## Database Configuration" |
| `command_approval` | Show/edit a shell command | Approve a generated rm -rf |

## Synergies

### ask_cards + pipeline_advance
```
ask_cards (approval gate) → pipeline_advance (advance if approved)
```

### ask_cards + artifact_create
```
ask_cards (collect structured config) → artifact_create (persist as config artifact)
```

### ask_cards + spawn_agent
```
ask_cards (collect task parameters) → spawn_agent (with structured prompt from results)
```

## Anti-patterns
1. **DO NOT use ask_cards for single yes/no questions.** That's what chat is for.
2. **DO NOT ask questions answered by USER.md or AGENTS.md.** Read the context first.
3. **DO NOT chain more than 8 cards without a break.** Split into batches.
4. **DO NOT skip the condition/exitIf/branches flow control.** Use it. Linear chains are boring.
5. **DO NOT use confirm for routine operations.** Only for destructive, irreversible, or high-cost actions.
6. **ALWAYS use type_confirm for deletion operations.** confirmWord = something specific, not "CONFIRM".
