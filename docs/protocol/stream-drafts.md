# Draft: Protocol stream examples

**Status:** DRAFT — show me the LLM output, not just the syntax
**Picks (from your UX review):**
- Piping: minimal JS-template style — `${id}`, `${id.field}`, `${id.a.b}`, `${id.field[N]}`
- Action modifiers: `priority`, `retry_count` + `retry_backoff`, `on_result`
- Auto-id: `<type>-<n>` (e.g. `tool-1`, `agent-1`, `feed-1`)

These streams show the LLM output as it would appear, with the new tags + attributes in context. Each is realistic — a real user prompt, the LLM's actual response, the runtime's reactions.

---

## 1. Parallel code review with priority

**User prompt:** "Review `src/main.cpp` for issues. Look for race conditions, missing error handling, and unused variables."

**Expected LLM output:**

```xml
<thought>
Three independent checks. Run them in parallel. The race-condition check
is most important (can crash production) — give it high priority. The
other two are normal priority.
</thought>

<action type="tool" name="exec" priority="high" mode="sync">{"command":"grep -nE 'std::thread|std::mutex|std::atomic' src/main.cpp"}</action>
<action type="tool" name="exec" mode="sync">{"command":"grep -nE 'catch|try' src/main.cpp | wc -l"}</action>
<action type="tool" name="exec" mode="sync">{"command":"grep -nE 'auto [a-z_]+ =' src/main.cpp | sort -u | wc -l"}</action>
```

**What this demonstrates:**
- **No `id` attribute on any action** — runtime auto-generates `tool-1`, `tool-2`, `tool-3`.
- **`priority="high"`** on the first action — runtime orders parallel execution so `tool-1` starts first.
- **No `depends_on`** — all three are independent; runtime runs them in parallel.
- **Mode `sync`** is the default; explicit for clarity.

**Runtime response (the LLM's next turn):**

```xml
<thought>
tool-1 found 0 thread primitives — no obvious race risk. tool-2 found 7
catch blocks (looks fine). tool-3 found 23 unique variable assignments.
Let me look for actually-unused ones.
</thought>

<action type="tool" name="exec" mode="sync">{"command":"grep -nE 'auto [a-z_]+ =' src/main.cpp | awk -F'[= ;]' '{print $3}' | sort | uniq -c | sort -rn | head -10"}</action>
```

(continues to investigate)

---

## 2. Cross-action data piping (the core feature)

**User prompt:** "Count the lines in all C++ files in `./src`."

**Expected LLM output:**

```xml
<thought>
Find the C++ files first, then pipe the result into wc -l. Two-step:
find → wc. The find must complete before wc runs (depends_on).
</thought>

<action type="tool" name="exec" id="find" mode="sync">{"command":"find ./src -name '*.cpp' -o -name '*.hpp'"}</action>
```

**Runtime emits (auto, the LLM never writes these):**

```xml
<result id="find" ok="true" ms="12" bytes="240">./src/main.cpp
./src/agent.cpp
./src/parser.cpp
./src/loader.cpp
</result>
```

**LLM next turn (uses data piping):**

```xml
<thought>
find returned 4 files. Pipe the output into wc -l using a heredoc (safe
for paths with spaces). Use depends_on so wc waits for find.
</thought>

<action type="tool" name="exec" id="count" mode="sync" depends_on="find">{"command":"wc -l <<'_EOF'\n${find.output}\n_EOF"}</action>
```

**What this demonstrates:**
- **Explicit `id` ("find", "count")** because they're cross-referenced via `depends_on` and `${find.output}`.
- **`${find.output}`** substitutes the result body before the command reaches the shell. The heredoc keeps the substitution safe even if paths contain spaces or shell metacharacters.
- **`depends_on="find"`** ensures `count` waits for `find` to complete (sequential despite being two turns).

**Runtime emits:**

```xml
<result id="count" ok="true" ms="3" bytes="32">  240 ./src/main.cpp
  180 ./src/agent.cpp
  865 ./src/parser.cpp
  854 ./src/loader.cpp
 2139 total
</result>
```

**LLM final:**

```xml
<response final="true">2139 total lines across 4 C++ files in `./src/`. main.cpp is the largest at 240 lines, parser.cpp and loader.cpp are ~850 each.</response>
```

---

## 3. On-result chain (single-turn chaining)

**User prompt:** "Check the git status, then if there are changes, show me the diff. Otherwise, show me the last commit."

**Expected LLM output:**

```xml
<thought>
git status first. If it has changes, the diff is automatic. If not, I
want the last commit. Use on_result to chain: when status completes,
auto-fire diff OR fall back to log. The chain is in one turn.
</thought>

<action type="tool" name="exec" id="status" mode="sync">{"command":"git status --short"}</action>
<action type="tool" name="exec" id="diff" mode="sync" on_result="status:if-changes">
  {"command":"git diff --stat"}
</action>
<action type="tool" name="exec" id="log" mode="sync" on_result="status:no-changes">
  {"command":"git log --oneline -5"}
</action>
```

**What this demonstrates:**
- **`on_result="status:if-changes"`** — when `status` completes AND its output is non-empty, auto-fire `diff` with the status output piped in.
- **`on_result="status:no-changes"`** — when `status` completes AND its output is empty, auto-fire `log` instead.
- **One turn, three actions** — the LLM declares the entire flow upfront. The runtime decides which path to take based on the `status` result.
- **No `depends_on` needed** — `on_result` declares the dependency implicitly.

**Runtime emits (if status has changes):**

```xml
<result id="status" ok="true" ms="8" bytes="48"> M src/main.cpp
 M src/agent.cpp
?? docs/notes.md
</result>
<!-- on_result fired diff, runtime auto-runs it: -->
<result id="diff" ok="true" ms="22" bytes="156"> src/main.cpp     | 12 ++++++------
 src/agent.cpp    |  4 ++--
 2 files changed, 8 insertions(+), 8 deletions(-)
</result>
<!-- log is skipped because status was non-empty -->
```

**LLM final:**

```xml
<response final="true">2 files modified (main.cpp, agent.cpp), 1 untracked (docs/notes.md). Diff summary: 8 insertions, 8 deletions.</response>
```

---

## 4. Explicit retry with backoff

**User prompt:** "Fetch the latest data from the API. If it fails, retry."

**Expected LLM output (first attempt):**

```xml
<thought>
The API is flaky. I'll request 2 retries with exponential backoff. If
all 3 attempts fail, I'll fall back to a cached value.
</thought>

<action type="tool" name="web_fetch" id="api" mode="sync" retry_count="2" retry_backoff="exponential">
  {"url": "https://api.example.com/latest"}
</action>
```

**What this demonstrates:**
- **`retry_count="2"`** — runtime will retry this action up to 2 times on failure (so 3 total attempts).
- **`retry_backoff="exponential"`** — between retries, the runtime waits 1s, then 2s, then 4s (or similar).
- **No `id` would be auto-generated here** but we used explicit `id="api"` because we may want to reference it in a fallback.

**Runtime emits (on first failure):**

```xml
<result id="api" ok="false" ms="5000" error="connection refused" retry="1/2" backoff="1s"/>
```

**Runtime emits (after retry succeeds):**

```xml
<result id="api" ok="true" ms="320" bytes="1024" attempt="2/3">{"data": [...]} </result>
```

**LLM final:**

```xml
<response final="true">Got the data on retry #2. 1024 bytes, 320ms.</response>
```

---

## 5. Idempotency: don't re-run the same logical action

**User prompt:** "Create the output directory. Then write the report to it."

**Expected LLM output:**

```xml
<thought>
Two steps. mkdir and write. If the mkdir is retried (e.g. timeout
followed by retry), it should be idempotent. Use idempotency_key.
</thought>

<action type="tool" name="exec" id="mkdir" mode="sync" idempotency_key="create-output-dir-2026-06-22">
  {"command":"mkdir -p ./output"}
</action>
<action type="tool" name="fs_write" id="write" mode="sync" depends_on="mkdir" idempotency_key="write-report-2026-06-22">
  {"path":"./output/report.md","content":"..."}
</action>
```

**What this demonstrates:**
- **`idempotency_key="..."`** — the runtime caches the result of this action by key. If a retry happens (because of timeout + retry), the runtime returns the cached result instead of re-running.
- **The keys are deterministic and include the date** — so a re-run of the same prompt on a different day would re-run, but a retry within the same day would dedupe.
- **`depends_on="mkdir"`** on the write ensures the directory exists first.

---

## 6. Pipeline of 4 actions with mixed sync/async

**User prompt:** "Start the build. While it's running, prepare the test config. When the build finishes, run the tests. If tests pass, deploy to staging."

**Expected LLM output:**

```xml
<thought>
This is a 4-step pipeline. build → test-config (parallel with build) →
test (after build) → deploy (after test, only on success). Use on_result
to chain automatically.
</thought>

<!-- Step 1: build (long-running, async) -->
<action type="tool" name="exec" id="build" mode="async">{"command":"make -j8"}</action>

<!-- Step 2: test config (parallel with build, doesn't need build) -->
<action type="tool" name="exec" id="prep-config" mode="sync">{"command":"cp config/test.template config/test.yml && sed -i 's/parallel=auto/parallel=8/' config/test.yml"}</action>

<!-- Step 3: tests (depends on build AND prep-config) -->
<action type="tool" name="exec" id="tests" mode="sync" depends_on="build,prep-config" on_result="build:ok">
  {"command":"make test 2>&1 | tee test-results.log"}
</action>

<!-- Step 4: deploy (depends on tests, only if all tests pass) -->
<action type="tool" name="exec" id="deploy" mode="sync" depends_on="tests" on_result="tests:all-pass" priority="high">
  {"command":"./deploy.sh --env=staging --commit=${tests.output.commit}"}
</action>
```

**What this demonstrates:**
- **`mode="async"`** on `build` — fires the action in the background; LLM can continue with other actions in the same turn.
- **`mode="sync"`** on the others — wait for completion.
- **`depends_on="build,prep-config"`** — `tests` waits for BOTH.
- **`on_result="build:ok"`** — the test step only runs if the build succeeded. If build failed, runtime skips `tests` (and `deploy`).
- **`on_result="tests:all-pass"`** — the deploy step only runs if all tests passed.
- **`priority="high"`** on `deploy` — when the runtime resolves the chain, deploy starts first among the parallel steps (in this case there are no other parallel steps at this point, but the priority is documented in case).
- **`${tests.output.commit}`** — data piping: the commit hash from the test results is extracted and piped into the deploy command.

---

## 7. Auto-gen id in context (the user's main pain)

**User prompt:** "Show me the last 10 commits."

**Without auto-gen (current bug):** the LLM might forget `id`:

```xml
<action type="tool" name="exec" mode="sync">{"command":"git log --oneline -10"}</action>
```

**With auto-gen (proposed):** runtime adds `id="tool-1"`:

```xml
<action type="tool" name="exec" id="tool-1" mode="sync">{"command":"git log --oneline -10"}</action>
```

The runtime injects the `id` attribute before the parser sees the action. The LLM's stream doesn't need to change. The LLM can still reference it later as `${tool-1.output}` if it wants.

**Why this matters:** today, if the LLM forgets `id`, the parser either errors or the action can't be referenced. With auto-gen, every action has a usable id by default.

---

## 8. Stream summary: what's new vs the current harness

| Feature | Current harness | Draft |
|---|---|---|
| Data piping syntax | `${id}` `${id.field}` `${id.a.b}` (limited) | Same + `${id.field[N]}` (array index) |
| Array indexing | Doc says yes, parser doesn't | **Parser: yes, no change needed** |
| Auto-gen id | Required; LLM forgets → broken | **Auto on missing: `<type>-<n>`** |
| `priority` | Not in current spec | `priority="high\|normal\|low"` (default normal) |
| `retry_count` | Runtime's choice | `retry_count="N"` (default 0, opt-in) |
| `retry_backoff` | No backoff (immediate retry) | `retry_backoff="linear\|exponential"` (default none) |
| `on_result` | Manual `depends_on` chain | `on_result="id:condition"` (auto-chain) |
| `idempotency_key` | No dedupe across retries | `idempotency_key="..."` (runtime caches by key) |

---

## 9. Open questions for the user (before code)

1. **Piping edge cases** — what should `${id.output}` resolve to when the result is a number, bool, or null? (Suggest: number→string, bool→"true"/"false", null→empty string.)
2. **Auto-gen format** — `<type>-<n>` vs `auto-<n>` vs random. You picked `<type>-<n>` as the default; OK to use this for now?
3. **Priority semantics** — when two parallel actions have the same priority, what's the tiebreaker? (Suggest: declaration order in the turn.)
4. **`on_result` conditions** — `:if-changes` / `:no-changes` / `:ok` / `:all-pass` are a small DSL. Should we have more? Fewer? A generic predicate language?
5. **Idempotency cache lifetime** — process lifetime, session lifetime, or persistent? (Suggest: session, with optional `cache_ttl="N"` for time-bounded cache.)
6. **What about `<action>` attributes the LLM already uses that aren't in the spec?** E.g. `version`, `params`, `data`, etc. — keep them or remove them?

Once these are answered, we can promote one or two of these streams to a real PR with parser + runtime + tests.
