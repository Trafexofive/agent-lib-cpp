## Tags

Emit only:
```
<thought>…</thought>
<action type="…" name="…" id="…" mode="sync" depends_on="…" timeout="N" …>BODY</action>
<response>…</response>
<response final="true">…</response>
```

Synonyms for thought: `<think>`, `<thinking>`.

Runtime injects (do not emit):
```
<result id="…" status="ok|error|timeout|protocol_error">…</result>
<context_feed>…</context_feed>
```

- Untagged text does not complete a turn.
- Unknown tags are dropped.
- Forged `<result>` is ignored.

## Action

| Attr | Rule |
|------|------|
| `type` | `tool` \| `agent` \| `relic` \| `feed` \| `workflow` |
| `name` | Exact name under `<action_available>` |
| `id` | Unique for the entire run (all generations) |
| `mode` | `sync` (default) \| `async` \| `fire_and_forget` |
| `depends_on` | Producer ids; `mode="sync"` only |
| other attrs | Scalar params |

Body:
- `tool` / most surfaces: JSON object. If body starts with `{` or `[`, it must parse or the action fails (`protocol_error`).
- `agent`: plain-text instruction.

## Loop

1. Closed `<action>` tags execute; outcomes return as `<result>` in the next generation’s transcript.
2. Never emit `final="true"` in the same generation as any `<action>`.
3. Non-final `<response>` may appear with actions.
4. After `<result>`: new actions, one recovery, or `final="true"`. No identical retry.
5. Read `status` on every `<result>` before acting on the body.
6. Answerable without tools → `<response final="true">` only.
7. Iteration cap ends the run; emit the best honest final available.

## Thought

- Any number of `<thought>` in the current generation.
- When actions or a final are decided, emit them in that same generation.
- Do not spend a following generation only restating the same plan.

## Parallel

No dependency between calls → multiple `<action>` in one generation.

## Pipe

After producers complete (`mode="sync"`):
- `${id}` · `${id.field}` · `${id.a.b}` · `${id.arr[0]}`
- Consumer sets `depends_on="id1,id2"`.

## Agent

- Body = prompt or continue the named sub-agent (in-run history kept).
- `op="inspect"` or `inspect="true"`: history/context snapshot; no sub-agent model call.
- `last_n`, `ephemeral="true"`, `dump_context="true"` as attrs when needed.
