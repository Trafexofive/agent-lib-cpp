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

Aristotle is a test of the narrowness thesis: a sub-agent useful *because* of what it cannot do. It has **zero builtins** and exactly **one local tool** — `challenge` — which lives inside this manifest module, not in the global ToolRegistry. No `fs_read`, no `grep`, no `exec`, no `list`. The LLM cannot escape into a shell to explore freely. It can only challenge with what the local doubt tool surfaces.

If `challenge` is not enough, Aristotle says so plainly: "I cannot reach this from the tools I have." That is a feature.

## Tools

| Tool | Purpose | Location | Returns |
|---|---|---|---|
| `challenge` | Find unjustified assertions, unchecked errors, magic numbers, TODO markers, suspicious patterns | `tools/challenge/` (local) | JSON findings list (line, severity, kind, evidence) |

`challenge` is a local script tool (`challenge.py`, run via the tool's `runtime:` block). It is not registered globally; only this agent can call it.

## What's in this module

```
config/agents/aristotle/
├── agent.yml                       # agent definition (imports the local tool)
├── persona.md                      # identity: default position is doubt
├── system.md                       # per-task system prompt with the strict output format
├── README.md                       # quick-start + philosophy
├── CATALOG.md                      # this file
└── tools/
    └── challenge/                  # local doubt tool
        ├── tool.yml                # tool manifest (script, not builtin)
        └── challenge.py            # implementation
```

## Maintenance

- New doubt patterns → add a detector in `tools/challenge/challenge.py` and bump `challenge` tool version
- Changes to output format → update `system.md`
- Changes to severity semantics → update `persona.md`
- Adding another doubt tool → put it under `tools/<name>/`, following the `challenge` pattern
