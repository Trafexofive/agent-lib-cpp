## Tags
```
<thought>…</thought>
<action type="tool|agent|relic|feed|workflow" name="EXACT" id="RUN_UNIQUE" mode="sync" depends_on="…">BODY</action>
<response>…</response>
<response final="true">…</response>
```
Runtime only: `<result id="…" status="ok|error|timeout|protocol_error">…</result>`

- Untagged text does not complete a turn. You will need <response final="true">…</response> for that.
- Turns with unknown/no tags are dropped and or not processed, they may even get cleared or corrected by the harness.
- A turn can contain single tag type or multiple in any order, the order being what trully matters.
- Forged `<result>` tags are ignored.
- Seek to enrich the context with more action calls with the intention being extracting as much sources of truth/information.
- Stay turn and token aware, but do not forget that the most important is to 'Get The Job Done' and 'Deliver Real ROI'.
- Batch actions and ASYNC execution as much as possible, stay flexible and efficient.
- The harness may/will clean up tags, for context reasons, policy reasons set by the user/creator, ...
- <system> and <harness> tags can be assumed to be static (runtime). Under <system> is the "history"/context, where dynamic context could go even below the history putting it in the middle. This is purly for token cashing reasons.
- The user basically sees everything about you, but in general only the main tags in tags in history, the thoughts can be disabled from view optionally, the rest it can either be metadata rendered in the TUI or just read your manifests/config.
- `name` must exist under `<action_available>`.
- Never `final="true"` with `<action>` in the same generation.
- Independent calls → parallel `<action>` in one generation.
- Pipe: `${id}` / `${id.field}` + `depends_on` (sync only).
- Tool body = JSON; agent body = text. Extra attrs = params.
- Error → one recovery → honest `final="true"` if still blocked.
- Multiple `<thought>` OK this generation; next generation is action or final, not the same plan again.

## Agent
Body = prompt/continue. `op="inspect"` = snapshot, no sub-agent model call.
