# Draft: Protocol expansion — new tags, new attributes, new semantics

**Status:** DRAFT — brainstorming + prototyping, no code
**Author:** <pending review>
**Scope:** the XML protocol the LLM emits (`<response>`, `<action>`, `<thought>`, `<result>`) and the attributes they carry
**Companion:** the current harness at `manifests/harness/default.md`

---

## 0. Why this draft exists

The current protocol is tight: 4 tags, 6 action attributes, strict parser. It works. But it has gaps that hurt at scale:

1. **Resume is blind.** When the user runs `cortex-mk3 -c`, the LLM has no way to know "where was I?" — it sees the prior conversation but no progress markers. Resume can pick up but can't pick up *cleanly*.
2. **No cost visibility.** Tokens are spent silently. The user has no signal until the bill.
3. **No source citation.** When the LLM says "the docs say X," it's inline text. No structured way to point at a file:line.
4. **No handoff between sub-agents.** A sub-agent that wants to give the parent a result has to do it via `<response>`. There's no formal "I am done, here's my conclusion, continue from here" primitive.
5. **No partial results.** A long-running tool blocks the LLM. There's no way to stream intermediate output and let the LLM react.
6. **No memory.** The LLM has the chat history. There's no "remember this for next session" primitive.

This draft proposes 6 new tags, 5 new attributes, and 2 new cross-cutting mechanisms. Each section is self-contained: motivation, syntax, example LLM output, parser impact, open questions.

---

## 1. New tags

### 1.1 `<plan>` — declare multi-step intent upfront

**Motivation:** Today the LLM commits to one action at a time. A 5-step task takes 5+ turns. The user has no way to know "this is going to take 5 steps" until it's done. With `<plan>`, the LLM declares the full intent in turn 1, then marks each step done as it completes.

**Syntax:**
```xml
<plan>
  <step id="p1" status="pending">Read the schema.yml to find the failing field</step>
  <step id="p2" status="pending" depends_on="p1">Grep for the field name across the codebase</step>
  <step id="p3" status="pending" depends_on="p2">Fix the field in the schema</step>
  <step id="p4" status="pending" depends_on="p3">Run the test suite to confirm the fix</step>
</plan>
```

**Turn flow:**
- Turn 1: LLM emits `<plan>` then `<action>` for step 1.
- Turn 2: LLM emits `<plan_done step="p1"/>` then `<action>` for step 2.
- ...
- Turn N: LLM emits `<plan_done step="p4"/>` then `<response final="true">Done.</response>`.

**Open questions:**
- Should the plan be required, or optional (for short tasks)?
- What happens if the LLM diverges from the plan mid-execution? Auto-revise? Warn? Ignore?
- How does the plan render in the TUI? (Suggest: a checklist at the top of the viewport, dimmed if pending, bright when done.)

### 1.2 `<plan_done step="pN"/>` — mark a plan step complete

**Motivation:** Pairs with `<plan>`. The LLM emits one of these before each `<action>` to mark the prior step done. Self-closing, no body.

**Syntax:** `<plan_done step="p1"/>` — single self-closing tag, references the `id` of the `<step>` it completes.

**Open questions:**
- Should the parser enforce that `<plan_done>` is only emitted for steps in the prior `<plan>`? Or trust the LLM?
- Should this be optional (LLM can just emit the next action)? Or required?

### 1.3 `<checkpoint>` — save point for resume

**Motivation:** Resume (`-c`) is blind. The LLM has to read the entire prior transcript to know where it is. With `<checkpoint>`, the LLM can mark progress: "I am at step 3 of 5, my last successful action was X, my next action will be Y." On resume, the LLM sees the checkpoint and continues from there.

**Syntax:**
```xml
<checkpoint name="after-fixing-schema">
  <state>step=3, last_action=e2, last_result=ok</state>
  <next>Run the test suite to confirm the fix</next>
</checkpoint>
```

**Resume behavior:** When `cortex-mk3 -c` loads, the runtime reads the most recent `<checkpoint>` and includes it in the system prompt. The LLM sees: "Resumed from checkpoint 'after-fixing-schema' (state: ...). Continue with: Run the test suite to confirm the fix."

**Open questions:**
- Should `<checkpoint>` be auto-emitted by the runtime, or always LLM-emitted? (Suggest: always LLM-emitted, the runtime just stores and replays them.)
- How many checkpoints to keep? Last N? All in the session?
- How does this interact with the existing `renderedHistory` replay on resume?

### 1.4 `<cite path="..." line="..." source="..."/>` — structured citation

**Motivation:** Today the LLM says "the docs say X" or "looking at line 42" inline. No structured citation. With `<cite>`, the LLM emits a tag that points to a specific file:line:source. The TUI can render citations as clickable links (in terminals that support it) or as styled references.

**Syntax:** `<cite path="./src/main.cpp" line="42" source="code"/>` — points to file:line with a source label (e.g. "code", "doc", "feed", "imported_file").

**Open questions:**
- Should `<cite>` be inline (inside a `<response>`) or top-level?
- Should the runtime verify the path/line exists? Or trust the LLM?

### 1.5 `<cost tokens="N" usd="0.001"/>` — declare cost

**Motivation:** The user has no cost visibility until the bill. With `<cost>`, the LLM can declare cost per response. The runtime accumulates per-session and warns on thresholds.

**Syntax:** `<cost tokens="N" usd="0.001"/>` — emitted as a self-closing tag, ideally once per turn (or per `<response>`).

**Open questions:**
- Should this be emitted by the LLM (estimated) or computed by the runtime (actual)? (Suggest: both. LLM estimates, runtime computes the actual.)
- What about cost limits? If a session exceeds $X, should the runtime auto-pause? (Suggest: yes, with a configurable budget per session.)

### 1.6 `<handoff to="agent-name" reason="..."/>` — formal sub-agent handoff

**Motivation:** Sub-agents today return via `<response>`. No formal "I am done, here is my conclusion, the next agent should start from X" primitive. With `<handoff>`, the parent can transfer state cleanly to a child, and the child can transfer back.

**Syntax:**
```xml
<handoff to="cortex-manifest-expert" reason="needs schema review">
  <state>reviewed code, found 3 BLOCKERs, awaiting schema validation</state>
</handoff>
```

**Open questions:**
- Should handoff preserve context (prior conversation) or pass a clean slate? (Suggest: preserve — the child sees the parent's context up to the handoff point.)
- Should handoff be one-way (parent → child) or two-way (child → parent)? (Suggest: two-way, with `<return>` for the child to hand back.)

---

## 2. New action attributes

The current `<action>` attributes: `type`, `name`, `id`, `mode`, `depends_on`, `timeout`. Adding 5 more:

### 2.1 `priority="high|normal|low"` — order parallel actions

**Motivation:** When multiple sync actions are emitted in one turn, they run in parallel. Today the order is undefined. With `priority`, the LLM can say "I want the schema lookup first, even if the others don't depend on it" — useful for ordering parallel work that the LLM thinks is most important.

**Syntax:** `<action type="tool" name="exec" id="e1" priority="high">...</action>`

**Open questions:**
- Does `priority` affect runtime ordering, or just TUI rendering?
- If two parallel actions both have `priority="high"`, what's the tiebreaker?

### 2.2 `cost_estimate="high|normal|low"` — LLM declares expected cost

**Motivation:** The LLM knows "this exec will take 5 seconds" or "this will call the API 100 times." With `cost_estimate`, it can warn the runtime. The runtime can show "this action is expensive" in the TUI or refuse if the session budget is too low.

**Syntax:** `<action type="tool" name="exec" id="e1" cost_estimate="high">...</action>`

**Open questions:**
- Is `cost_estimate` enforced (runtime refuses if exceeds budget) or advisory (TUI shows a warning)?
- What are the buckets? Tokens? Wall time? Both? Provider-specific cost?

### 2.3 `retry_count="N"` and `retry_backoff="linear|exponential"` — explicit retry

**Motivation:** Today, retries are the runtime's choice. The LLM has no way to say "this action can be retried 3 times with exponential backoff." With explicit `retry_count` and `retry_backoff`, the LLM declares the retry policy.

**Syntax:** `<action type="tool" name="web_fetch" id="w1" retry_count="3" retry_backoff="exponential">...</action>`

**Open questions:**
- Should the runtime enforce `retry_count`, or is this advisory?
- What's the default if not specified? (Suggest: 0 retries — the LLM must opt in.)

### 2.4 `on_result="e1:callback-action"` — callback on result

**Motivation:** The LLM emits a sync action and gets a result. The result triggers a follow-up action. Today, the follow-up is a separate turn. With `on_result`, the LLM can chain: when action `e1` completes, automatically fire `e2` (using the result of `e1` as input).

**Syntax:** `<action type="tool" name="exec" id="e2" on_result="e1:run-tests">...</action>` — when `e1` completes, run `run-tests` with the result of `e1` piped in.

**Open questions:**
- Is this just sugar for `depends_on`? Or genuinely new (a single-action turn can chain automatically without a turn boundary)?
- How does this interact with parallel actions?

### 2.5 `idempotency_key="..."` — dedupe retries

**Motivation:** If an action is retried, the runtime might run it twice. With `idempotency_key`, the LLM can say "this is the same logical action; don't run it twice." The runtime caches results by key.

**Syntax:** `<action type="tool" name="fs_write" id="w1" idempotency_key="write-readme-2026-06-22">...</action>`

**Open questions:**
- What's the cache lifetime? Session? Process? Persistent?
- What about partial failures (the action started but didn't complete)?

---

## 3. New cross-cutting mechanisms

### 3.1 `<action_partial>` and `<result_partial>` — streaming tool output

**Motivation:** A long-running tool blocks the LLM until completion. With partial results, the LLM can react mid-execution: "this is taking too long, abort and try a different approach" or "I see the first 10 lines, that's enough, cancel and return."

**Syntax:**
```xml
<!-- LLM emits -->
<action_partial type="tool" name="exec" id="e1" mode="sync" stream="incremental">{"command":"find . -name '*.cpp'"}</action_partial>

<!-- Runtime emits (the LLM never writes this; it reads these) -->
<result_partial id="e1" seq="1">src/main.cpp</result_partial>
<result_partial id="e1" seq="2">src/foo.cpp</result_partial>
... (many more) ...
<result_partial id="e1" seq="N" final="true">...last line...</result_partial>
```

**Open questions:**
- Does this require a new `mode` value, or is it always-on for long-running tools?
- Can the LLM emit a follow-up action mid-stream (e.g., "abort e1, run e2 instead")?
- What's the protocol for partial failures?

### 3.2 `<error>` and `<fallback>` — explicit error semantics

**Motivation:** Today, tool failures arrive as `<result ok="false" error="...">`. The LLM has to figure out the recovery. With explicit `<error>` and `<fallback>`, the runtime can declare the failure mode and the LLM can declare a fallback action.

**Syntax:**
```xml
<!-- Runtime emits on failure -->
<error id="e1" severity="transient|fatal|warning" reason="connection-refused" retryable="true"/>

<!-- LLM can declare a fallback chain -->
<action type="tool" name="web_fetch" id="w1" mode="sync">
  {"url": "https://api.example.com/data"}
  <fallback>
    <action type="tool" name="web_fetch" id="w2" mode="sync">{"url": "https://api-mirror.example.com/data"}</action>
  </fallback>
</action>
```

**Open questions:**
- Is `<fallback>` a new construct, or just sugar for emitting two actions with `depends_on`?
- What's the distinction between `severity="transient"` (retry) and `severity="fatal"` (give up)?

---

## 4. Cross-cutting concerns

### 4.1 Memory: `<remember>`, `<recall>`, `<forget>`

**Motivation:** The LLM has the chat history. There's no "remember this for next session" primitive. With explicit memory tags, the LLM can persist facts to long-term storage and recall them later.

**Syntax:**
```xml
<remember scope="user|preferences|project" key="preferred-test-runner" value="ctest"/>
<recall scope="user|preferences|project" key="preferred-test-runner"/>
<forget scope="user|preferences|project" key="preferred-test-runner"/>
```

**Open questions:**
- Where is memory stored? Local file? Server? Cross-session?
- How is it scoped? Per-user, per-project, per-agent?
- Privacy: what's stored, what's redacted?

### 4.2 Multi-modal: `<image>`, `<audio>`, `<attachment>`

**Motivation:** Today the protocol is text-only. With multi-modal tags, the LLM can attach images, audio, and other files. The runtime can inline them in the prompt for vision-capable models.

**Syntax:**
```xml
<image path="./screenshot.png" caption="The error dialog"/>
<audio path="./recording.wav" duration="30s"/>
<attachment path="./report.pdf" type="application/pdf"/>
```

**Open questions:**
- Are these emitted by the LLM (the LLM generates them) or by the runtime (the LLM cites them)?
- How is the file passed to the LLM? Inline base64? URL? Provider-specific format?
- What about size limits?

---

## 5. Implementation sketch (for reference, NOT this PR)

When we move from draft to code, the work is roughly:

1. **Parser** — extend `src/protocol/parser.cpp` to recognize the new tags. Each tag is a TokenEvent kind. The parser emits events, doesn't store state.
2. **Harness** — extend `manifests/harness/default.md` to document the new tags. The LLM needs to know the syntax.
3. **Runtime** — extend `src/core/agent.cpp` to handle the new tags. `<plan>` updates the in-memory plan; `<plan_done>` marks a step done; `<checkpoint>` is saved to the session; `<cite>` is rendered; `<cost>` is summed; `<handoff>` is a structured sub-agent call.
4. **Tests** — extend `src/testing/parser_test.cpp` with feed/produce tests for each new tag. Also: integration tests in `session_test.cpp` for `<checkpoint>` round-trip.
5. **Backwards compatibility** — every new tag is optional. The LLM never *must* emit them. The parser ignores unknown tags gracefully (as it does today).

Estimated scope: ~600 lines of new code (parser + runtime + tests), 0 lines removed.

---

## 6. What to ship first (proposed priority)

If we ship one per release:

| # | Tag/attribute | Why first |
|---|---|---|
| 1 | `<cite>` | Smallest change, highest value (every agent already cites things) |
| 2 | `<plan>` + `<plan_done>` | Resume gets dramatically better; user can see intent upfront |
| 3 | `<checkpoint>` | Resume gets a "where was I?" anchor; pairs with `<plan>` |
| 4 | `priority` on `<action>` | Small change, makes parallel work more useful |
| 5 | `idempotency_key` on `<action>` | Pairs with retry; makes retries safe |
| 6 | `<cost>` | User-facing; needs budget infrastructure first |
| 7 | `<handoff>` | Sub-agent flows get cleaner; bigger change |
| 8 | `<action_partial>` / `<result_partial>` | Long-running tools; biggest parser change |
| 9 | `<remember>` / `<recall>` | Cross-session memory; needs storage infrastructure |
| 10 | `<image>` / `<attachment>` | Multi-modal; depends on provider support |

---

## 7. Companion files (also drafts, not code)

None yet. When we promote any of these to a real PR, the companion would be:
- `manifests/harness/versions/default.v2026-XX-XX-<tag>.md` — a new harness version documenting the new syntax
- `src/protocol/parser.cpp` + tests — the parser change
- `src/core/agent.cpp` + tests — the runtime change
- A one-line bump in `docs/TICKETS.md` to mark each shipped slice

---

## 8. Open questions for the user (before we code any of this)

1. **Scope**: are all 6 new tags + 5 new attributes + 2 cross-cutting mechanisms in scope, or should we narrow to a subset?
2. **First slice**: which of the 10 items in §6 do you want to ship first?
3. **Backwards compatibility**: do all new tags need to be optional (LLM may not emit them), or are some required?
4. **Parser strategy**: extend the existing parser, or fork it (a new parser for the new tags, with a switch)?
5. **Testing**: what level of test coverage is needed? Unit tests on the parser, integration tests on the runtime, both, or end-to-end?
6. **Harness updates**: do we add to the existing `default.md` or create a new versioned harness (e.g. `default.v2026-XX-XX-plan-and-checkpoint.md`)?
7. **Backward compat for `<plan>` and `<checkpoint>`**: do existing agents need to be updated, or do they just keep working without the new tags?

Once these are answered, we can promote the prioritized slices to design docs and then to code in atomic commits.
