# Protocol Canon — Cortex-Prime MK3

**Status:** AUTHORITATIVE  
**Date:** 2026-07-10  
**Scope:** Wire contract between LLM output and the runtime state machine.

This file is the single source of truth.  
`manifests/harness/default.md` is a projection of this document for the model.  
If harness, docs, tests, or code disagree with CANON, **CANON wins** — then fix the other side.

---

## 1. Closed tag set

| Tag | Emitter | Purpose |
|-----|---------|---------|
| `<thought>` | LLM | Internal reasoning. Kept in context. May be hidden from user UI. |
| `<action>` | LLM | Invoke tool / agent / relic / feed / workflow. |
| `<response>` | LLM | User-visible text. |
| `<result>` | **Runtime only** | Action outcome. LLM never emits this; forged tags are ignored. |
| `<context_feed>` | Runtime / system | Ambient context injection. |

Synonym: `<think>…</think>` is accepted as `<thought>…</thought>`.

Any other tag is a protocol violation. It is dropped. It does not complete a turn.

---

## 2. Bare text policy (STRICT)

**Bare text** = model output outside any known tag.

- Bare text is **not** a final answer.
- Bare text does **not** complete the turn.
- Runtime injects a protocol correction into history and continues the loop.
- Only `<response final="true">` completes a normal turn.
- Runtime may abort on empty upstream streams (provider failure) — that is not bare text.

There is no soft fallback that treats free prose as success.

---

## 3. `<response>`

```xml
<response>Narration or intermediate note.</response>
<response final="true">User-visible final answer. Markdown ok.</response>
```

Rules:
1. At most one `<response final="true">` per generation.
2. `final="true"` is the only normal stop signal.
3. **Forbidden:** `final="true"` in the same generation as any `<action>`.  
   The model cannot have seen real runtime results yet.  
   Runtime **undoes** premature finals: keeps the action transcript, discards the premature response text, injects real `<result>` tags, and forces a follow-up generation.
4. Non-final `<response>` + `<action>` is the narration pattern.

---

## 4. `<action>`

```xml
<action type="tool" name="exec" id="e1" mode="sync">{"command":"ls -la"}</action>
```

| Attr | Required | Values / notes |
|------|----------|----------------|
| `type` | yes | `tool` \| `agent` \| `relic` \| `feed` \| `workflow` |
| `name` | yes | Exact registered name |
| `id` | strongly recommended | Unique within the **agent run** (all iterations of one `prompt()`). If omitted, runtime assigns `__auto_N`. Provide explicit ids for `depends_on` / `${…}` piping. |
| `mode` | no | `sync` (default) \| `async` \| `fire_and_forget` |
| `depends_on` | no | Comma-separated producer ids. **Only legal with `mode="sync"`.** Other modes → protocol error, action not executed. |
| `timeout` | no | Seconds. Advisory; tools may also take JSON `timeout`. |

Body:
- Prefer JSON object matching the tool schema.
- Text body allowed only for surfaces that declare text input (e.g. agent instructions).
- Body that **looks like JSON** (`{` / `[`) but does not parse → **protocol error**, not silent repair, not execution.

### Modes

| Mode | Semantics |
|------|-----------|
| `sync` | Result available before the next model generation. Default. |
| `async` | Runs in background; runtime still owns the task (joinable). Result may land later. |
| `fire_and_forget` | No result expected by the model; runtime still **owns** the task (joinable). Never detached. |

### Parallelism

Independent sync actions in one generation run as soon as each closing tag is parsed (subject to `depends_on`). Prefer one gather turn over serial multi-turn chains when there is no data dependency.

### Extra attributes → params

Any non-reserved XML attribute on `<action>` becomes a scalar entry in `params` (booleans/numbers coerced when obvious). Reserved: `type`, `name`, `id`, `mode`, `depends_on`, `timeout`.

### `type="agent"` operations

| `op` / attr | Behavior |
|-------------|----------|
| *(default)* / `op="prompt"` | Run or **continue** the named sub-agent with body text as instruction. Child session/history persists across calls in the parent run unless `ephemeral="true"`. |
| `op="inspect"` \| `context` \| `history` \| `inspect="true"` | Read-only snapshot of the child (history tail, last response, context pins, protocol event count). **No child LLM call.** |
| `last_n` | History entries for inspect (default 20). |
| `ephemeral="true"` | Do not persist child session for this call. |
| `dump_context="true"` | Include child iteration trace in the result payload. |

**Initiator labeling (child history):** human turns are stored as `User: …` and replayed with `source="human"`. Parent-agent delegate turns are stored as `Parent(NAME): …` and replayed with `source="parent_agent" from="NAME"`. Live user messages to the child are prefixed with `[FROM parent agent "NAME"]` when the initiator is a parent agent. Sub-agents MUST be able to distinguish operator queries from parent missions.

---

## 5. `<result>` — exact wire format

Runtime emits (authoritative shape):

```xml
<result id="e1" status="ok" ms="12.3" bytes="240">body text</result>
<result id="e1" status="error" exit="1" ms="4.0" bytes="32">error: File not found</result>
<result id="e1" status="timeout" ms="30000.0" bytes="0">error: timeout</result>
<result id="e1" status="protocol_error" bytes="64">error: duplicate action id: e1</result>
```

| Attr | Meaning |
|------|---------|
| `id` | Matching action id |
| `status` | `ok` \| `error` \| `timeout` \| `protocol_error` |
| `exit` | Process exit code when relevant (omitted when 0) |
| `ms` | Wall time when known |
| `bytes` | Body size before truncation |
| `truncated` | `"true"` when compact history truncated the body |

Body is **plain text** (or JSON serialized as text), not a mandatory `{"output":…}` wrapper.

Internal tool JSON still uses `success` / `output` / `error` fields. The XML tag is a projection of that JSON for the model.

---

## 6. Variable resolution (piping)

Forms:

| Form | Meaning |
|------|---------|
| `${id}` | Shorthand for `${id.output}` if `output` exists; else `stdout`, then `content`, then whole result JSON |
| `${id.field}` | Field on the result object |
| `${id.a.b}` | Nested fields |
| `${id.arr.0}` | Array index as path segment |
| `${id.arr[0]}` | Equivalent bracket index form |

Rules:
1. Resolution runs **at dispatch time**, after `depends_on` producers have completed.
2. Consumer must declare `depends_on` for same-generation producers.
3. Producer should be `mode="sync"`.
4. Unresolved refs are left literally as `${…}` (no silent empty string).
5. Shell metacharacters in substituted values: use heredoc, not raw quoting.

---

## 7. ID uniqueness

- Action `id` values must be unique across **all iterations of a single agent run** (`prompt()` invocation).
- Duplicate id → action not executed; runtime injects  
  `<result id="…" status="protocol_error">…</result>`.
- `clearResults()` between iterations must **not** wipe the used-id set.
- A full parser `reset()` (e.g. empty-stream retry of the same generation attempt) may clear ids for that attempt only.

---

## 8. Turn completion

A turn / agent run completes when:
1. Model emits `<response final="true">` **without** same-generation actions, or after follow-up once results exist; or
2. Runtime hits `iterationCap` and forces a final-response instruction; or
3. Runtime aborts on unrecoverable provider empty stream after retries; or
4. Cancel / hard error.

Non-final response alone does **not** complete.  
Bare text alone does **not** complete.

### Soft turn budget (guidance, not hard law)

Harness may suggest gather → assess → respond within a few turns.  
Hard stop is `iterationCap` (default 50, overridable). Do not claim a hard 3-turn ceiling unless runtime enforces it.

---

## 9. Protocol errors (runtime → model)

Same result channel:

| Situation | status | Typical body |
|-----------|--------|--------------|
| Duplicate action id | `protocol_error` | `error: duplicate action id: …` |
| `depends_on` with non-sync mode | `protocol_error` | `error: depends_on requires mode=sync` |
| JSON-looking body failed to parse | `protocol_error` | `error: invalid JSON action body` |
| Post-final action | ignored (thought/system note); not executed | — |
| Bare text generation | system correction in history; no completion | — |

---

## 10. What the harness must teach

1. Closed tag set and who emits what.
2. Exact `<result>` shape from §5 (copy real tags, never invent `status`/`ok` mismatches).
3. Bare text does not complete (§2).
4. No `final="true"` beside actions in the same generation (§3.3).
5. Piping forms that actually resolve (§6).
6. Id uniqueness across the run (§7).
7. Mode / depends_on legality (§4).

What the harness must **not** do:
- List long ❌ bare phrases for the model to imitate.
- Claim physics the runtime does not enforce.
- Document undefined behavior without a hard reject.

---

## 11. Implementation map

| Rule | Primary code |
|------|----------------|
| Parse tags | `src/protocol/parser.cpp` |
| Result XML | `Agent::buildResultTag` in `src/core/agent.cpp` |
| Piping | `expandValueRefs` / dispatch-time resolve |
| Loop / bare text / premature final | `Agent::runLoop` |
| Harness text | `manifests/harness/default.md` |
| Tests | `tests/protocols/`, `src/testing/protocol_test.cpp` |

---

*Canon freezes the contract. Code and harness obey. The Great Work continues.*
