# Aristotle — The Doubter

A specialized Cortex-Prime MK3 sub-agent whose only job is to find what's wrong, what's unjustified, and what the author hasn't thought through. Default position: doubt.

## Philosophy

Aristotle is a test of the **narrowness** thesis: a sub-agent that is useful *because* of what it cannot do. It has **zero builtins** and two purpose-built doubt tools. No `fs_read`, no `grep`, no `exec`. The LLM cannot escape into a shell to explore freely. It can only challenge with what the doubt tools surface.

If the doubt tools are not enough, Aristotle says so plainly. That is a feature, not a bug.

## When to use Aristotle

Reach for `aristotle` when the task is:

- **Code review** — "is this function actually correct?"
- **Audit** — "find the unchecked errors in this file"
- **Verify a claim** — "the author says this is thread-safe; is it?"
- **Find the weak point** — "what's the worst thing in this PR?"
- **Triage a bug** — "given this code, where would it break?"

Do not use Aristotle when the task is to *fix* or *write* code. Aristotle surfaces doubts; it does not resolve them.

## How to invoke

Add `aristotle` to the `import.agents:` list of any agent that should be able to call it:

```yaml
import:
  agents: [default, aristotle]
```

Then from the model:

```xml
<action type="agent" name="aristotle" id="r1" mode="sync">
  Doubt src/core/agent.cpp — focus on the runLoop method.
</action>
```

Aristotle returns a severity-tagged findings table. The caller is responsible for acting on it.

## What's in this module

```
manifests/agents/aristotle/
├── agent.yml      # agent definition (zero tools imports)
├── persona.md     # identity: default position is doubt
├── system.md      # per-task system prompt with the strict output format
├── CATALOG.md     # this file
└── README.md      # quick-start + philosophy
```

## Maintenance

- New doubt patterns → add a detector in `src/tools/builtins/challenge.cpp` and bump `challenge` tool version.
- Changes to the output format → update `system.md` (the format is part of the contract).
- Changes to severity semantics → update `persona.md` (BLOCKER / CONCERN / NIT).
