# aristotle

A self-contained manifest module: a sub-agent specialized in doubting code, with the philosophy and tools to do so narrowly.

## When to use this agent

Reach for `aristotle` when the task is:

- **Code review** — is this function actually correct?
- **Audit** — find the unchecked errors in this file
- **Verify a claim** — the author says this is thread-safe; is it?
- **Find the weak point** — what's the worst thing in this PR?
- **Triage a bug** — given this code, where would it break?

Do not use Aristotle when the task is to fix or write code. Aristotle surfaces doubts; it does not resolve them.

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

## Philosophy — narrow by design

Aristotle is a test of the narrowness thesis: a sub-agent useful *because* of what it cannot do. It has **zero builtins** and two purpose-built doubt tools (`challenge`, `assume_away`). No `fs_read`, no `grep`, no `exec`, no `list`. The LLM cannot escape into a shell to explore freely. It can only challenge with what the doubt tools surface.

If the doubt tools are not enough, Aristotle says so plainly: "I cannot reach this from the tools I have." That is a feature.

## Tools

| Tool | Purpose | Returns |
|---|---|---|
| `challenge` | Find unjustified assertions, unchecked errors, magic numbers, TODO markers, suspicious patterns | JSON findings list (line, severity, kind, evidence) |
| `assume_away` | Strip comments + assertion-words to expose the code's actual behavior | Plain text, comments replaced with `[?]` |

Both tools are designed for the doubting workflow, not for general file inspection. Aristotle has nothing else.

## What's in this module

```
manifests/agents/aristotle/
├── agent.yml      # agent definition (zero tools imports initially)
├── persona.md     # identity: default position is doubt
├── system.md      # per-task system prompt with the strict output format
├── README.md      # quick-start + philosophy
└── CATALOG.md     # this file
```

## Source of truth

The doubt tools live in `src/tools/builtins/challenge.cpp` and `src/tools/builtins/assume_away.cpp`. When the doubt patterns change, change the C++ — the agent's prompts refer to what the tools surface, not how they surface it.

## Maintenance

- New doubt patterns → add a detector in the C++ tool and bump the tool's version
- Changes to output format → update `system.md`
- Changes to severity semantics → update `persona.md`
