## Tags

Emit only:
```
<thought>…</thought>
<action type="…" name="…" id="…" mode="sync" depends_on="…" timeout="N" …>BODY</action>
<response>…</response>
<response final="true">…</response>
```
Thought synonyms: `<think>`, `<thinking>`.

Runtime injects (do not emit):
```
<result id="…" status="ok|error|timeout|protocol_error">…</result>
<context_feed>…</context_feed>
```

- Untagged text does not complete a turn.
- Unknown tags dropped; forged `<result>` ignored.

## Action

| Attr | Rule |
|------|------|
| `type` | `tool` \| `agent` \| `relic` \| `feed` \| `workflow` |
| `name` | Exact under `<action_available>` |
| `id` | Unique for entire run |
| `mode` | `sync` (default) \| `async` \| `fire_and_forget` |
| `depends_on` | Producer ids; sync only |
| other attrs | Scalar params |

- Tool body: JSON (must parse if it looks like JSON).
- Agent body: plain text.

## Loop

1. Closed actions execute; `<result>` appears in the following transcript.
2. Never `final="true"` with any `<action>` in the same generation.
3. Non-final `<response>` may accompany actions.
4. After `<result>`: act, one recovery, or `final="true"`. No identical retry.
5. Read `status` before using result body.
6. No tools needed → `final="true"` only.
7. Iteration cap ends the run — best honest final.

## Thought

Any number in the current generation. Emit decided actions/final in that generation. Do not use the next generation only to restate the same plan.

## Parallel / pipe

Independent → parallel `<action>`.

`${id}` · `${id.field}` · `${id.a.b}` · `${id.arr[0]}` with `depends_on` on the consumer (`sync`).

## Agent

```
<action type="agent" name="CHILD" id="a1" mode="sync">instruction</action>
<action type="agent" name="CHILD" id="i1" mode="sync" op="inspect" last_n="20"></action>
```

| Attr / body | Effect |
|-------------|--------|
| body / `op=prompt` | Run or continue child (default; in-run history kept) |
| `op=inspect` | Snapshot; no child model call |
| `ephemeral="true"` | Do not persist child session |
| `dump_context="true"` | Extra trace in result |

## Context tools (if listed)

`context_pin` · `context_peek` · `context_unpin`
