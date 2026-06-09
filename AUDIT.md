# MK3 Remaining Work — Raw Audit

```
core/
  http/
    - [ ] curl_easy_perform blocks main thread — spinner freezes, can't type during streaming
        - [ ] replace with curl_multi + select() in main loop
        - [ ] or spawn curl on std::thread + thread-safe token queue (promptAsync — tried, thread never reached prompt() body)
    - [x] no HTTP retry on rate-limit/network errors → iterative retry loop with exponential backoff
    - [ ] no connection reuse (new curl handle per request)
    - [ ] no streaming timeout detection (hangs silently if SSE stalls)

  agent/
    - [ ] no thread-safety on protocolActions_ / protocolResults_ / rawLlOutput_ — data race when async LLM added
    - [ ] dispatchTool and executeScriptTool both access tools_ without lock
    - [x] bare-text-as-response fallback skips JSON synthesis → JSON synthesis REMOVED
    - [x] iteration loop pushes ALL prior iterations into chat history → history cap enforced
    - [ ] no max-context-token guard — can exceed provider limits
    - [ ] context_feeds_ accumulated but never pruned
    - [ ] no sub-agent result streaming — sub-agent responses arrive as monolithic blocks
    - [ ] system prompt rebuilds from scratch per iteration (tool schemas, harness, persona) — harness cached, tools not
    - [ ] model parameter overrides in manifest not validated against provider capabilities
    - [x] no rate-limit backoff for retryable LLM errors → iterative retry with exponential backoff
    - [ ] config.yml model/provider overrides not validated — silent fallthrough on miss

  tools/
    - [ ] built-in tool dispatch is synchronous — blocks SSE stream for large `exec` outputs
    - [x] no tool timeout enforcement → system `timeout` command prepended to every exec
    - [ ] sandbox_launcher not wired to `executeScriptTool` (sandbox flag ignored for script tools)
    - [ ] no tool output size limit — massive outputs crash on JSON serialization
    - [ ] tool parameter validation only in schema — no runtime type checking
    - [ ] no parallel tool execution for built-in tools (only script tools are threaded)
    - [ ] `list` results field key is `results` (not `output`) — fragile field extraction heuristic
    - [ ] no tool result caching — repeated identical calls re-execute
    - [ ] tool error messages embedded in JSON — no structured error codes

  protocol/
    - [ ] parser loses `<response ` prefix in some edge cases (raw.md shows `final="true">`)
    - [ ] no streaming parse — waits for full action/response close tag
    - [ ] no validation of XML well-formedness — malformed tags silently discarded
    - [ ] context_feed events parsed but not rendered or stored
    - [ ] thought events parsed but discarded (no rendering path)
    - [ ] no protocol version negotiation — hardcoded to current format
    - [ ] error events stored in history_ string form — no structured error handling

tui/
  renderer/
    - [ ] spinner stops during curl_easy_perform — unfixable without async LLM
    - [ ] no expand/collapse for action blocks — full output always shown
    - [ ] no action state tracking — pending/running/done/failed not rendered
    - [ ] result block cache reset on incremental update forces full protocol re-render
    - [ ] Markdown rendering of partial text can produce garbled output (live MD during streaming)
    - [ ] no scroll position persistence across prompts — always jumps to bottom
    - [ ] historyLines grows unbounded — no ring buffer or size cap
    - [ ] diff rendering still has edge cases when viewport shifts (squashing)
    - [ ] no code syntax highlighting in Markdown code blocks
    - [ ] table alignment breaks on narrow terminals (<80 cols)
    - [ ] no image rendering path (data URIs, file:// references)

  input/
    - [ ] blocks on stdin during non-streaming input — can't process background events
    - [ ] no paste detection or large-paste handling
    - [ ] history only persists on clean exit — lost on crash
    - [ ] no multi-byte character support (UTF-8 beyond ASCII)
    - [ ] Ctrl-C during prompt construction kills entire process instead of clearing line

  statusbar/
    - [ ] no token count / context utilization display
    - [ ] no provider/model shown in status bar
    - [ ] no connection status indicator (connected, rate-limited, error)

session/
  - [ ] sessions.json grows unbounded — no pruning or rotation
  - [ ] session save on every turn — could batch writes
  - [ ] no session export/import (shareable sessions)
  - [ ] iteration.md and raw.md overwritten each run — no timestamped archives

manifests/
  - [ ] no manifest schema validation — invalid YAML silently produces broken agent
  - [ ] no manifest hot-reload — requires restart
  - [ ] import.env values not validated for required keys
  - [ ] no manifest inheritance or composition
  - [ ] tool.yml runtime field fallback fragile — checks `runtime` then `entrypoint`
  - [ ] sub-agent path resolution brittle — checks ./agents/<name>/ first, then manifest path

harness/
  config/harness/default.md
    - [ ] LLM often ignores "wrap everything in XML" — emits bare text
    - [ ] data piping (${id.field}) never used by LLM
    - [ ] parallel execution guidance not followed — LLM defaults to sequential
    - [ ] non-final <response> as thinking not documented in examples
    - [ ] stop condition guidance not strong enough — LLM over-searches
    - [ ] no example showing sub-agent delegation with depends_on
    - [ ] guardrails section too abstract — needs concrete "you will be rejected if" statements

tests/
  - [ ] no end-to-end integration test (full prompt roundtrip)
  - [ ] no crash-on-exit regression test (valgrind/ASAN)
  - [ ] no renderer output snapshot tests
  - [ ] no protocol parser unit tests (malformed input, edge cases)
  - [ ] no tool execution tests (timeout, sandbox, error paths)
  - [ ] no manifest loading tests (invalid YAML, missing fields, circular imports)
  - [ ] no session save/load roundtrip test
  - [ ] no harness prompt template test (injection, truncation)

docs/
  - [/] AGENDA.md created — needs regular updates
  - [ ] no API reference for Agent public interface
  - [ ] no developer quickstart (build, run, test, manifest authoring)
  - [ ] no changelog
  - [ ] no architecture diagram or data flow doc

build/
  - [ ] no ASAN build target in Makefile
  - [ ] no debug/release build profiles
  - [ ] no install target
  - [ ] jsoncpp vendored but not tracked in dependency list
  - [ ] curl linked dynamically — no static or bundled option for portability
```
