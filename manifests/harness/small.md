## Tags
```
<thought>…</thought>
<action type="tool|agent|relic|feed|workflow" name="EXACT" id="RUN_UNIQUE" mode="sync" depends_on="…">BODY</action>
<response>…</response>
<response final="true">…</response>
```
Runtime only: `<result id="…" status="ok|error|timeout|protocol_error">…</result>`

- Untagged text does not complete a turn.
- `name` must exist under `<action_available>`.
- Never `final="true"` with `<action>` in the same generation.
- Independent calls → parallel `<action>` in one generation.
- Pipe: `${id}` / `${id.field}` + `depends_on` (sync only).
- Tool body = JSON; agent body = text. Extra attrs = params.
- Error → one recovery → honest `final="true"` if still blocked.
- Multiple `<thought>` OK this generation; next generation is action or final, not the same plan again.

## Agent
Body = prompt/continue. `op="inspect"` = snapshot, no sub-agent model call.
