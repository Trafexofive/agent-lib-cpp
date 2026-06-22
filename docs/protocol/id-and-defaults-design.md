# Draft: id auto-gen, harness tweaks, action_log, modifier defaults

**Status:** DRAFT — brainstorming + prototyping, no code
**Scope:** four related improvements to the protocol and the harness prompt

---

## 0. Why these four belong together

The current harness has three pain points:
1. **LLMs forget `id`**, breaking `depends_on`, `on_result`, and `${...}` references. Auto-gen is the right fix; the user wants the harness to own this.
2. **The harness prompt is verbose** and not optimized for token efficiency or for the new modifiers we just drafted.
3. **There's no way to reference past actions** after the result has been consumed by the next step. An `action_log` would let the LLM look back.
4. **Defaults for modifiers are scattered** (runtime hard-coded, harness undocumented, no per-agent override). A `modifiers_default:` block would centralize them.

This draft proposes designs for all four, in atomic slices.

---

## 1. Auto-gen id — owned by the harness

### 1.1 Current state

The parser requires `id` on every action. The LLM often forgets. When it does, the action can't be referenced.

### 1.2 Proposed behavior

**The harness owns id generation.** Specifically:

1. The harness prompt tells the LLM: "You can omit `id`. The runtime will auto-generate. For cross-references (`depends_on`, `on_result`, `${...}`), you must provide `id` explicitly."
2. The runtime auto-generates `id` when missing. Format: `<type>-<n>` where `<type>` is `tool`/`agent`/`relic`/`feed` and `<n>` is a per-session counter.
3. The auto-gen happens BEFORE the parser sees the action — the parser doesn't know whether `id` was explicit or auto.
4. The auto-gen is silent by default; no warnings. (Optional: warn once via `<system>` correction if the LLM omits id for a cross-referenced action.)

### 1.3 Why "owned by the harness" rather than "owned by the parser"

The harness is the contract between the LLM and the runtime. The LLM only reads the harness. So:
- The behavior is documented in the harness prompt — the LLM knows it can rely on auto-gen.
- The implementation is in the runtime — but the runtime is just a backend detail.
- The parser is a low-level component that doesn't need to know about the auto-gen policy.

This separation: **harness = the contract**, **parser = the syntax**, **runtime = the executor**.

### 1.4 Format choice

`<type>-<n>` (e.g. `tool-1`, `agent-2`, `feed-3`):
- Type information preserved (the LLM can tell what kind of action it is from the id).
- Counter is per-session; resets on `--no-session`.
- Matches the user's preferred default from the prior UX review.

### 1.5 Example (current vs proposed)

**Current (LLM forgets id):**
```xml
<action type="tool" name="exec" mode="sync">{"command":"ls"}</action>
<!-- result: parser error or action can't be referenced -->
```

**Proposed (auto-gen):**
```xml
<action type="tool" name="exec" mode="sync">{"command":"ls"}</action>
<!-- runtime injects: id="tool-1" -->
<result id="tool-1" ok="true" ms="3" bytes="...">...</result>
```

**Proposed (LLM provides id for cross-reference):**
```xml
<action type="tool" name="exec" id="list-files" mode="sync">{"command":"ls"}</action>
<action type="tool" name="exec" id="count" mode="sync" depends_on="list-files">{"command":"wc -l <<'_EOF'\n${list-files.output}\n_EOF"}</action>
```

### 1.6 Implementation sketch

In the runtime, when an action is about to be parsed:
```cpp
if (action.id.empty()) {
    action.id = action.type + "-" + std::to_string(actionCounter[action.type]++);
}
```

Per-session counter, per-type. The runtime already tracks action results by id; auto-gen just makes sure every action has one.

---

## 2. Harness tweaks — making the prompt effective and efficient

### 2.1 Current state

The current harness (`manifests/harness/default.md`, 217 lines) is:
- Verbose (lots of examples that could be smaller)
- Doesn't document the new modifiers (`priority`, `retry_count`, `retry_backoff`, `on_result`, `idempotency_key`)
- Doesn't mention auto-gen id
- Has the "FAILED TURNS" section that takes ~15 lines for what could be ~3
- Has "COLD START" advice that's 2 paragraphs

### 2.2 Proposed changes

**1. Add a "Modifiers" section** documenting `priority`, `retry_count`, `retry_backoff`, `on_result`, `idempotency_key` (from the prior draft).

**2. Add a "Cross-references" section** showing:
- `${id.output}` for piping
- `${id.field}` for nested JSON
- `${id.field[N]}` for array index
- `depends_on="id1,id2"` for serial
- `on_result="id:condition"` for chained branching

**3. Compress the "FAILED TURNS" section** from 4 examples to 1-2. The pattern is the same; one example is enough.

**4. Add a "Defaults" section** at the end:
```
DEFAULTS (when you omit an attribute):
  id           — runtime auto-generates as <type>-<n>
  mode         — sync
  depends_on   — none (parallel)
  timeout      — 60 seconds
  priority     — normal
  retry_count  — 0
  retry_backoff — none (immediate on retry)
  idempotency_key — none
  on_result    — none
```

**5. Add a "Self-check" block at the top** that's shorter:
```
⚠ First character of every turn: <  |  Bare text is STRIPPED. Silent.
```

**6. Move the "examples" section to a separate doc** (`docs/protocol/examples.md`) — keep the harness for protocol, not examples.

**7. Add a "HARD LIMITS" section:**
```
NEVER:
  - Bare text outside tags (stripped, silent)
  - Omit `id` on cross-referenced actions
  - Retry more than twice per failure
  - Loop on a plan; if stuck, emit <response final="true"> with what you have
```

### 2.3 Target size

Current: 217 lines.
Target: ~140 lines (35% smaller) by:
- Compressing FAILED TURNS (15 → 4 lines)
- Compressing COLD START (10 → 4 lines)
- Moving EXAMPLES to a separate doc (40 → 5 lines, just a pointer)
- Adding new sections (Modifiers, Defaults, Self-check) (~30 lines)
- Other compression (10 lines saved)

### 2.4 Versioning

The current `default.md` is the default. The tweaked version is `default.v2026-XX-XX-modifiers-and-defaults.md` — a new version. The runtime picks the latest version available. The `default.md` (untweaked) stays as a fallback.

Or, the user might prefer to just edit `default.md` in place. **Decision needed.**

### 2.5 Why a new version, not in-place

The harness is the LLM's contract. A new version lets us A/B test (some agents on old, some on new), and gives us a rollback point. Editing in place is simpler but loses the safety net.

---

## 3. `action_log` — referencing past actions

### 3.1 What it is

A feed (or tool) that takes a past action's `id`, stores the result + metadata, and lets the LLM reference stored entries by index.

### 3.2 Why it's useful

Today, when the LLM has many parallel actions, the results flow back but the LLM may want to reference them later — e.g., "compare result of action 3 to result of action 7." Today, the LLM has to either remember or use the explicit `id` reference. With many parallel actions, `id` gets long or repeated.

An `action_log` lets the LLM:
- Append a result to the log: `<action name="action_log">{"id": "list-files"}</action>`
- Reference by index: `${log.3.output}` is the 3rd stored action's output
- Get the list: `<action name="action_log" mode="query">{}</action>` returns the metadata of all stored actions

### 3.3 API shape

**Input modes:**
- **append**: `{"mode": "append", "id": "<action-id>"}` — store the result of the named action, return the assigned index.
- **get**: `{"mode": "get", "index": N}` — return the stored result at index N (or by id).
- **list**: `{"mode": "list"}` — return metadata for all stored entries (id, index, type, name, ms, ok).
- **clear**: `{"mode": "clear"}` — drop all entries.

**Output format:**
```json
{
  "success": true,
  "action_log": {
    "count": 3,
    "entries": [
      {"index": 0, "id": "list-files", "type": "tool", "name": "exec", "ok": true, "ms": 12, "bytes": 240},
      {"index": 1, "id": "count",     "type": "tool", "name": "exec", "ok": true, "ms": 3,  "bytes": 32},
      {"index": 2, "id": "deploy",    "type": "tool", "name": "exec", "ok": false, "ms": 5000, "error": "connection refused"}
    ]
  }
}
```

### 3.4 Cross-reference syntax

Once a result is in the log, the LLM can reference it by index:
```xml
<action type="tool" name="exec" mode="sync" depends_on="log:3">
  {"command":"echo ${log.3.output}"}
</action>
```

The `log:N` syntax is a new kind of dependency: "wait for entry N in the action_log to be populated." This is in addition to `id` references (`depends_on="e1"`) and `on_result` (`on_result="status:if-changes"`).

### 3.5 Storage

The action_log is a feed (not a tool). State lives in the session:
- `~/.cortex/sessions/<id>.json` gets a new field: `action_log: [{index, id, type, name, ok, ms, bytes, error, body}]`
- On `-c` resume, the log is restored.
- `clear` mode drops entries (or marks them as cleared).

### 3.6 Naming (RESOLVED)

**`action_log`** — confirmed by the user.

### 3.7 Permissions (RESOLVED)

**Silent.** `action_log.clear` does not trigger `ask_tool`. The LLM has full freedom. The log starts cleared at session start. The user's reasoning: the log is session-scoped and not high-stakes; asking permission adds friction for no benefit.

---

## 4. `modifiers_default:` — per-agent defaults for action modifiers

### 4.1 Why

When the LLM omits a modifier, what happens?
- `mode` — defaults to `sync` (hard-coded)
- `depends_on` — defaults to `none` (parallel)
- `timeout` — defaults to 60s (hard-coded)
- `priority` — defaults to `normal` (proposed)
- `retry_count` — defaults to 0 (proposed)
- `retry_backoff` — defaults to `none` (proposed)
- `idempotency_key` — defaults to none (proposed)
- `on_result` — defaults to none (proposed)

These are scattered. The user wants a centralized way to override them per-agent.

### 4.2 Proposed: `modifiers_default:` block in the agent manifest

```yaml
modifiers_default:
  # Defaults for ALL actions unless the LLM overrides
  defaults:
    mode: sync
    timeout: 60
    priority: normal
    retry_count: 0
    retry_backoff: none

  # Per-tool-name overrides (LLM still wins, but these are the floor)
  per_tool:
    web_fetch:
      retry_count: 2
      retry_backoff: exponential
      timeout: 30
    exec:
      timeout: 300
      retry_count: 0
    fs_write:
      idempotency_key_template: "write-{path}-{date}"
```

**Resolution order** (later wins):
1. Hard-coded runtime defaults
2. `modifiers_default.defaults` (agent manifest)
3. `modifiers_default.per_tool.<name>` (agent manifest)
4. Explicit attributes in the `<action>` tag (LLM-emitted)

### 4.3 Where the block goes

In the agent manifest, as a top-level key (sibling of `context:`, `sandbox:`, `import:`).

### 4.4 Alternative: just document the defaults in the harness (RESOLVED — picked this)

The user picked the simpler alternative: just add a "DEFAULTS" section to the harness prompt. No `modifiers_default:` block in the manifest. The LLM sees the defaults and can override explicitly.

Trade-off: per-agent override of defaults (e.g. "for this agent, all `web_fetch` retries 3 times") isn't possible. If we need this later, add a manifest block then.

The harness prompt will have a clear "DEFAULTS" section:
```
DEFAULTS (when you omit an attribute):
  id           — runtime auto-generates as <type>-<n>
  mode         — sync
  depends_on   — none (parallel)
  timeout      — 60 seconds
  priority     — normal
  retry_count  — 0
  retry_backoff — none
  idempotency_key — none
  on_result    — none
```

---

## 5. What to ship first (proposed priority)

| # | Slice | Effort | Impact | Status |
|---|---|---|---|---|
| 1 | Auto-gen id in the runtime | ~30 LOC | **High** (fixes the LLM-forgets-id bug) | In scope (v1) |
| 2 | Harness: "Modifiers" + "Cross-references" + "Defaults" sections | ~30 LOC + new harness version | **High** (LLM now knows about the new attributes) | In scope (v1) |
| 3 | ~~`modifiers_default:` block in the manifest + parser~~ | ~~50 LOC~~ | ~~Medium (per-agent flexibility)~~ | **Dropped** (per user pick — just harness docs) |
| 4 | `action_log` feed (the whole thing) | ~150 LOC | Medium (long-running tasks get cleaner) | Defer to v2 |
| 5 | Harness: compress FAILED TURNS, COLD START | ~5 LOC (just text changes) | Low | In scope (folded into slice 2) |

I propose: **ship 1, 2, 5 in one release** (the harness + id work — id auto-gen in the runtime, harness new sections + new versioned file, compress existing sections). **Defer 3 and 4 to a later release** (action_log is a bigger change; modifiers_default is deferred per user pick).

---

## 6. Open questions for the user

| # | Question | Resolution |
|---|---|---|
| 1 | Harness version strategy | **New versioned file** (`default.v2026-XX-XX-modifiers-and-defaults.md`). Keep `default.md` as-is. Runtime picks the latest. Rollback-safe. |
| 2 | `action_log` name | **`action_log`** (confirmed). |
| 3 | `action_log.clear` permission | **Silent.** Full freedom. Log starts cleared at session start. Per the user: "it can have full freedom, its made for it, it starts cleared anyhow." |
| 4 | `modifiers_default:` location | **Just harness docs.** No `modifiers_default:` block in the manifest. Defaults live in the harness prompt; LLM can override explicitly. No parser change. |
| 5 | Ship order | *open.* My pick: 1+2+5 in one release (id auto-gen + harness tweaks). Defer 4 (the `action_log`) to a later release. |
| 6 | Idempotency key templating | *open.* My pick: literal only for v1. Templating (`write-{path}-{date}`) is v2. |
| 7 | `on_result` condition DSL | *open.* My pick: small fixed set (`:ok`, `:err`, `:if-changes`, `:no-changes`, `:all-pass`) for v1. Generic predicate is v2. |

Questions 5, 6, 7 are still open. If you don't pick, I'll go with my defaults.

Once 5/6/7 are answered (or defaulted), I can promote the locked slices to a real PR with harness + parser + runtime + tests.
