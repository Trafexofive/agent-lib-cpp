# MK3 Audit Tickets

Compiled from a three-agent codebase audit. Tickets are grouped by area and
ordered by severity within each section. Severity:

- **P0** — silently breaks core flows; ship-blocker
- **P1** — documented features that don't work end-to-end
- **P2** — friction/QoL — works, but awkward or surprising
- **P3** — smell — fine for now, fix during nearby refactors

Source lines reference HEAD at the time of audit.

---

## 1. YAML parser (`src/core/mini_yaml.hpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| Y01 | P0 | Inline flow maps `{ provider: x, model: y }` stored as raw string, never parsed. Breaks `cognitive_engine.primary` on every modern manifest. | `mini_yaml.hpp:96` |
| Y02 | P0 | Block scalars `>` and `|` not handled — `description: >` stores `>` literally, indented lines become orphan children. | `mini_yaml.hpp` (no block-scalar code) |
| Y03 | P0 | `get()` returns `""` when key exists with no inline value — ignores caller's default, silently clears `cfg.provider` etc. | `mini_yaml.hpp:37` |
| Y04 | P0 | `nodeToJsonValue` uses `node.key` as fallback value for leaf nodes — corrupts tool input/output schemas. | `manifest_loader.hpp:466` |
| Y05 | P1 | Inline comments `# default` not stripped from values → `temperature: 0.7 # default` throws. | `mini_yaml.hpp:26` |
| Y06 | P3 | No anchor/alias (`&x` / `*x`) support — silently dropped as bare text. | `mini_yaml.hpp` |

---

## 2. Manifest loader (`src/core/manifest_loader.hpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| ML01 | P0 | `builtin/exec` syntax always fails — `/` triggers path branch, looks for nonexistent path, tool silently dropped. | `manifest_loader.hpp:186` |
| ML02 | P0 | `stoi`/`stod` in `loadAgentConfig` not in try/catch — typo in `temperature:` kills process. | `manifest_loader.hpp:64-74` |
| ML03 | P0 | Workflow loader uses CWD-relative `manifests/workflows/<n>.yml` — drops imports if binary run outside project root. | `manifest_loader.hpp:270` |
| ML04 | P1 | Sub-agent lookup tries only `./agents/<n>/agent.yml` and `../<n>/agent.yml` — module-co-located agents not found. | `manifest_loader.hpp:231-234` |
| ML05 | P1 | `loadBuiltinToolSchema` third search path contains a literal `*` wildcard that `fs::exists` won't expand. | `manifest_loader.hpp:448` |
| ML06 | P1 | `config:` section parsed but never consumed anywhere — `config/staging/overrides.yml` totally inert. | (search-wide) |
| ML07 | P1 | `runtime.timeout` field declared in manifests but has no `AgentConfig` field. | `manifest_loader.hpp:97-100` |
| ML08 | P2 | Only `kind: Agent` validated; other kinds never checked. | `manifest_loader.hpp:48` |
| ML09 | P2 | Three live tool-name conventions: bare (`exec`), path (`./tools/x/tool.yml`), broken-prefix (`builtin/exec`). Pick one. | manifest examples |
| ML10 | P3 | Every `load*` function re-reads + re-parses the same YAML file. | `manifest_loader.hpp` |
| ML11 | P3 | `readFile` duplicated in `manifest_loader.hpp` and `manifest_autoload.hpp`. | both files |

---

## 3. Manifest autoload (`src/core/manifest_autoload.hpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| MA01 | P0 | Autoloader creates sub-agents from `agent.yml` but never calls `loadTools/loadFeeds/loadRelics/loadSubAgents/loadWorkflows` on them — sub-agent `import:` totally ignored. | `manifest_autoload.hpp:90-100` |
| MA02 | P1 | `manifests/agents/assistant/agent.yml` uses `agent:` (singular) for sub-agents; loader reads `agents:` (plural). | `manifest_loader.hpp:227` vs manifest |
| MA03 | P2 | Duplicate tool registration when `--manifest` and `--manifest-dir` overlap — `seenTools` dedup is internal to autoloader only. | `main.cpp:744-761` |
| MA04 | P3 | `"manifests"` magic string default has no constant or shared definition. | `main.cpp:742` |

---

## 4. New manifest features needed

| ID  | Sev | Summary |
|-----|----|---------|
| MF01 | P1 | `extends:` keyword — manifest inheritance from a base agent.yml |
| MF02 | P1 | `disable_builtins:` first-class field at root of agent.yml (declarative complement to imperative `disable_builtin` tool) |
| MF03 | P2 | Manifest validation — surface typos / unknown fields / missing required fields |
| MF04 | P2 | First-class `kind: Module` or `kind: Library` for reusable-primitives manifests (decided later — out of scope for now) |
| MF05 | P2 | Path-resolution convention spec — pick one of: relative, `module://`, sibling-search; document and enforce |

---

## 5. Agent core loop (`src/core/agent.cpp` + `agent_tools.cpp` + `agent_session.cpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| AC00 | P0 | Session-restored tools must render real schemas in `<tools>`; never show `desc="See input_schema for parameters"` without the actual schema. | `agent.cpp`, `agent_tools.cpp` |
| AC01 | P1 | T03 — manifest `max_iterations` parsed into cfg but call-site order determines whether CLI or manifest wins. No "manifest-wins-unless-CLI-overrides" policy. | `manifest_loader.hpp:97`, `main.cpp` |
| AC02 | P1 | T06 — `systemPrompt_` populated once in constructor; `reload_manifests` never rebuilds it → hot-reloaded persona invisible until restart. | `agent.cpp:75-84`, `agent_tools.cpp:195` |
| AC03 | P1 | T19 — tool dedup against `__TOOL_SCHEMAS__` does raw substring match `name="..."` — fragile across quoting/whitespace variations. | `agent.cpp:641-655` |
| AC04 | P1 | `saveSession` always calls `create()` → overwrites `created` timestamp every save. | `agent_session.cpp:61-63` |
| AC05 | P1 | `undoLastInteraction` pops 2 entries blind — breaks pairing when last turn had multiple tool results. | `agent_session.cpp:92-97` |
| AC06 | P1 | Subagent invocation passes no sessionId → fresh blank context every delegation. | `agent.cpp:277` |
| AC07 | P1 | `provider` hardcoded `"deepseek"` in session metadata regardless of actual provider. | `agent_session.cpp:62` |
| AC08 | P2 | `buildChatPrompt` returns one `system` message with everything packed in — no role separation (`[system][user][assistant]...`) even though providers expect it. | `agent.cpp:592-596` |
| AC09 | P2 | History cap drops from front blindly — can land between paired `User:`/`Agent:` entries and corrupt context. | `agent.cpp:721-724` |
| AC10 | P2 | `iterations.md` / `raw.md` hardcoded to CWD — concurrent agents clobber. | `agent_session.cpp:13-33` |
| AC11 | P3 | `history_` stored as prefixed strings, parsed by `rfind` every prompt build — parallel `SessionRecord` struct unused. | `agent.cpp:726-758` |
| AC12 | P3 | `rawLlOutput_` accumulates across iterations with no per-iteration boundary marker. | `agent.cpp:208` |
| AC13 | P3 | `sanitize()` truncates `<response>` at first literal `</response>` substring (e.g., agent quoting the protocol). | `agent.cpp:802` |

---

## 5b. Harness / prompt contract

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| HP01 | P0 | Rewrite harness to be generic, not `exec`-centric. Examples must use available capability classes or be dynamically generated from the active tool surface. | `config/harness/default.md`, prompt builder |
| HP02 | P1 | Add/experiment with an "auto chain actions" steering surface: a small suggested action list/tree for common probes (e.g. tree/list, git status, targeted reads) generated from actual available tools, not hardcoded `exec`. | design |
| HP03 | P1 | Separate protocol rules from tool capability context. Keep protocol compact; make tool/relic/feed schemas accurate and prominent. | prompt builder |

## 6. Protocol parser (`src/protocol/parser.cpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| PP01 | P0 | `findClosingTag` does plain substring search — nested `<action><action></action></action>` chops at the inner closer. No depth counting. | `parser.cpp:199` |
| PP02 | P0 | `closingTagStart` off-by-one — formula assumes `closingPos` is at `<` of the closer, but `findClosingTag` returns `pos + closingTag.length()`. Truncates last char or includes byte of closer. | `parser.cpp:139,147` |
| PP03 | P0 | `injectResult` self-deadlocks: holds `mtx_` then calls `dispatchPending()` which re-acquires `mtx_`. | `parser.cpp:449-457` ↔ `parser.cpp:429` |
| PP04 | P0 | Async / fire-and-forget `emit()` runs without holding the lock that protects `history_`/`rawLlOutput_` — data race on every async turn. | `parser.cpp:383-407` |
| PP05 | P1 | `clearResults()` between iterations also wipes `usedActionIds_` → cross-turn duplicate-ID guard silently broken (contradicts the comment that says it persists). | `parser.cpp:494-499` |
| PP06 | P1 | T09 — `${step.output}` workflow variable resolver only resolves against action `id`s in `results_`. No `step.` prefix routing. | `parser.cpp:521` |

---

## 7. Workflow engine (`src/workflows/workflow_engine.hpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| WF01 | P1 | T18 — `depends_on` parsed by protocol parser for `<action>`, but workflow `<step>` never emits `depends_on`. `WorkflowStep` has no `dependsOn` field. | `parser.cpp:347-354` vs `workflow_engine.hpp:271-309` |
| WF02 | P1 | `maxRetries` and `timeout` on `WorkflowStep` parsed into struct, never enforced, never emitted in XML. | `workflow_engine.hpp:247-248` |
| WF03 | P1 | Workflow XML uses `id=` attributes; `${step.output}` references `step.` prefix nobody resolves. | combined |
| WF04 | P2 | Workflow MiniYaml parser only handles flat `key: value` and list items — block scalars / anchors silently dropped. | `workflow_engine.hpp:63-152` |

---

## 8. Feed engine (`src/feeds/feed_engine.hpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| FE01 | P0 | T13 — `refresh_seconds` parsed nowhere. Feeds polled synchronously in `buildSystemPrompt` every turn; no timer/thread/interval. | `feed_engine.hpp:94-202` |
| FE02 | P0 | T25 — `runtime: builtin` returns early with `success=true` but never calls `registerFeed()` → declared builtin feeds produce nothing. | `feed_engine.hpp:127-131` |
| FE03 | P1 | `setenv("CALL_TOOL", ...)` called AFTER `popen()` — child already forked, can't see the env var. | `feed_engine.hpp:222-229` |

---

## 9. Built-in tools (`src/tools/internal_tools.cpp` + `agent_tools.cpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| BT01 | P0 | T28 — `entrypoint` path in tool.yml passed straight to `popen` with no canonical/traversal check. Manifest can specify `../../etc/...`. | `agent_tools.cpp:227,68,78` |
| BT02 | P1 | T26 — `disable_builtin` reads `params["name"]`; LLMs commonly send `{"tool": "x"}` → silent fail with "name or names required". | `agent_tools.cpp:118-124` |
| BT03 | P1 | T21 — `enable_builtin` erases from `disabledBuiltins_` but never re-inserts into `tools_` map → tool absent from prompt and dispatch. | `agent_tools.cpp:126-131` |
| BT04 | P1 | `reload_manifests`, `disable_builtin`, `enable_builtin` handled before sandbox `validate()` → sandboxed agent can break out by reloading. | `agent_tools.cpp:18-27` |
| BT05 | P1 | `fs_write` returns `{success, path}` with no body key → `buildResultTag` emits empty `<result/>`. LLM can't confirm write. | `internal_tools.cpp:144-147`, `agent.cpp:175-196` |
| BT06 | P2 | `executeScriptTool` uses predictable `/tmp/cortex-tool-<name>.json` — concurrent same-tool calls clobber each other. | `agent_tools.cpp:71` |
| BT07 | P2 | `fs_read` offset/limit math: `ln-offset >= limit` underflows when `ln < offset`. | `internal_tools.cpp:123-126` |
| BT13 | P1 | Built-in/runtime-only tools need first-class prompt-building integration, not ad hoc manifest/schema drift. Especially context_pin/context_peek/context_unpin: builtins are for capabilities that cannot reasonably live in user/module scope, and their schemas/semantics must render accurately in `<action_available>`. | prompt builder, `agent_tools.cpp`, built-in schemas |

---

## 10. Sandbox (`src/sandbox/policy.hpp`, `src/core/sandbox_launcher.hpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| SB01 | P0 | `validate()` parses command from paramsJson via brittle substring scan — bypassable by field reordering. | `policy.hpp:43-58` |
| SB02 | P0 | `isWithinWorkspace` only checks raw prefix with no normalization → `workspace/../outside` passes. | `policy.hpp:138-146` |
| SB03 | P1 | `list`, `grep`, `web_fetch`, `context_pin/peek/unpin` skip `validate()` entirely. | `policy.hpp` (no entries) |
| SB04 | P1 | `launchDocker` sets no `--network` restriction — container has unrestricted outbound. | `sandbox_launcher.hpp:74-79` |
| SB05 | P3 | `/tmp/cortex-dockerfile-<name>` world-readable — multi-user host race. | `sandbox_launcher.hpp` |

---

## 11. Providers (`src/providers/`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| PR01 | P0 | `availableProviders()` lists 11; `createProvider()` returns nullptr for 5 (`sambanova`, `cerebras`, `hyperbolic`, `llm7`, `nvidia`). Callers trust the list. | `factory.hpp:17-23` |
| PR02 | P1 | No fallback-provider chain. `AgentConfig.fallbackProvider` exists, no code reads it. | search-wide |
| PR03 | P1 | HTTP retry only on 429 (and narrow 413 substring). 500/503 throw immediately. | `generic_openai.cpp:160-161` |
| PR04 | P2 | `curl_slist*` headers leaked if exception thrown post-init. | `generic_openai.cpp:102-174` |
| PR05 | P2 | `top_k` always sent when `supportsTopK=true`; no per-call override. | `generic_openai.cpp:29` |

---

## 12. TUI (`src/tui/`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| UI01 | P1 | Slash commands catalog lists 9 entries but no `execute()` dispatcher. `/model`, `/provider`, `/clear`, `/save`, `/load`, `/tools` all missing. | `slash_commands.hpp:21-32` |
| UI02 | P2 | `RenderMode::SEMI` is dead — falls through to `?` in switch, never handled in `render()`. | `renderer.hpp:52-55` |
| UI03 | P2 | `wrapText` counts bytes not codepoints → CJK / emoji wrap at wrong column. | `markdown.hpp:344-361` |
| UI04 | P2 | `Markdown::render()` `mutable` state — `mathBlock_`/`tableBuffer_` retain prior text on cached path. | `markdown.hpp:38-43,51-53` |
| UI05 | P2 | `ProtocolView::addResult` concatenates with stray newline; `isDelta` ignored. | `protocol.hpp:49` |
| UI06 | P3 | `visLen` counts CSI sequences (cursor moves, erases) as visible chars → wrong padding. | `protocol.hpp:157` |

---

## 13. Relics (`src/relics/`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| RL01 | P1 | `DockerRelicDispatcher::ensureContainerUp` blocks main thread up to 5s with `usleep`. | `docker_dispatcher.hpp:209-213` |
| RL02 | P1 | `dispatchHttp` leaks `curl_slist*` headers (no `curl_slist_free_all` after `curl_easy_cleanup`). | `builtin_relics.hpp:400-419` |
| RL03 | P2 | `loadRelic` YAML scanner ignores `endpoints:` and `healthPath:` keys → `def.endpoints` always empty. | `docker_dispatcher.hpp:62-78` |
| RL04 | P2 | No retry / back-off on relic HTTP calls; 5s hard timeout. | `docker_dispatcher.hpp` |

---

## 14. CLI (`main.cpp`)

| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| CL01 | P0 | Short option `'X'` aliased to three long options: `--iterations`, `--api-key`, `--init`. Only iterations works. | `main.cpp:305,330,341` |
| CL02 | P0 | `case 'K'` handler for `--api-key` is dead — long option maps to `'X'`. Server API key cannot be set via CLI. | `main.cpp:330 vs 377` |
| CL03 | P2 | Three different default models for `deepseek` in different code paths: `deepseek-v4-pro` (cli), `deepseek-chat` (provider config), `deepseek-chat` (factory default). | `main.cpp:59`, `generic_openai.hpp:113`, factory |
| CL04 | P2 | Config file `key=value` parser has no quoting → values with `=` (API keys, model names) silently truncated at first `=`. | `main.cpp:253-270` |

---

---

## 15. Stub audit (2026-06-11) — additional P0/P1 findings

Surfaced by a dedicated stub audit (Sonnet sub-agent) — write-only no-ops,
parsed-but-unused fields, returns-success-without-doing-the-work patterns.

### Tools (`src/tools/internal_tools.cpp`)
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| BT08 | P0 | `json` advertises `op: parse\|query\|validate` in description; code only handles `validate`/`pretty`/`minify` via `action:` key. `parse`/`query` silently error. | `internal_tools.cpp:161-171` |
| BT09 | P0 | `grep` returns `matches: 0/1` (rc==0 boolean) — a truthy flag mistyped as a match count. | `internal_tools.cpp:110` |
| BT10 | P0 | `web_fetch` silently downgrades `DELETE`/`PATCH` to `GET` (no method override outside POST/PUT branches). | `internal_tools.cpp:189-191` |
| BT11 | P1 | `ask_tool` returns error when invoked via HTTP server (no TTY). Manifests importing it for REST use are silently broken. | `internal_tools.cpp:287-289` |
| BT12 | P1 | `fs_read` iterates the entire file even when only a slice is requested (still loops to compute `total`). Plus the offset/limit math counts lines including those skipped. | `internal_tools.cpp:122-128` |

### Agent core
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| AC14 | P0 | `Agent::saveSession` hardcodes provider string `"deepseek"` in session metadata regardless of actual provider. | `agent_session.cpp:62` |
| AC15 | P0 | REPL dual session-write: `Agent::saveSession` writes via internal `sessionMgr_`; `main.cpp` then writes again via its own `sm` instance. Second write overwrites tool-call records. | `main.cpp:1170-1172`, `agent.cpp:578` |
| AC16 | P0 | `buildChatPrompt` packs everything (incl. user input via `<history>`) into one `system` message. User turn never appended to messages array — breaks native tool-call providers. | `agent.cpp:592-596` |
| AC17 | P1 | `dumpSessionArtifacts` unconditionally writes `iterations.md` / `raw.md` to CWD on every prompt invocation, including server requests. | `agent_session.cpp:13,31` |
| AC18 | P1 | `contextFeeds_` (LLM-injected via `<context_feed>`) re-injected each iteration but never saved to session file → lost on restart. | `agent.cpp:507-509`, `agent_session.cpp:60-82` |
| AC19 | P1 | `AgentContext::variables` and `AgentContext::actionResults` declared but never populated anywhere. | `types.hpp:163-164` |
| AC20 | P0 | `enable_builtin` removes from `disabledBuiltins_` but never re-inserts the `ToolDef` into `tools_`. Tool disappears from prompt + dispatch permanently until restart. | `agent_tools.cpp:127-128` |
| AC21 | P1 | `reloadManifests(backup=true)` creates an empty timestamped directory; no actual files copied into it. | `agent_tools.cpp:198-201` |

### Feeds (`src/feeds/feed_engine.hpp`)
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| FE04 | P0 | `setenv("CALL_TOOL")` runs AFTER `popen()` in `runScriptWithEnvStatic` — child process already forked, can't see the env var. Feed scripts using `$CALL_TOOL` always fail. | `feed_engine.hpp:223,228` |
| FE05 | P1 | `FeedEngine::injectIntoPrompt()` declared but never called — dead alternate injection path. | `feed_engine.hpp:74-84` |

### Dispatcher (`src/core/dispatch.hpp`)
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| DP01 | P0 | `dispatchAgent` sets `success = !output.empty()` — sub-agent returning empty `<response>` counted as failure. Empty responses are valid. | `dispatch.hpp:102` |
| DP02 | P1 | `ActionType::LLM_CALL` and `ActionType::INTERNAL` are parsed/typed but the dispatcher returns `"Unknown action type"` for both. | `dispatch.hpp:156-160`, `parser.cpp:711-712` |
| DP03 | P1 | `setenv("FEED_PARAMS")` is not thread-safe; concurrent feed dispatch stomps each other. | `dispatch.hpp:118` |

### Protocol parser
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| PP07 | P1 | `ParsedAction::timeout` is parsed but never enforced per-action. `waitForActions(deadline)` uses one global deadline. | `parser.cpp:388-389,519-522` |
| PP08 | P1 | `waitForActions(deadline)` calls `wait_for(deadline)` per future sequentially — total wait can be N×deadline, not absolute wall-clock. | `parser.cpp:519-522` |
| PP09 | P1 | `FIRE_AND_FORGET` actions spawn detached threads with no completion tracking. Any action that `depends_on` a fire-and-forget can stick in pending forever. | `parser.cpp:448-449` |

### Providers
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| PR06 | P0 | Default model `deepseek-v4-pro` (CLI) doesn't match DeepSeek provider config default `deepseek-chat`. No-manifest invocations may 404. | `main.cpp:59`, `generic_openai.hpp:111` |
| PR07 | P1 | `OpenAIProviderConfig::supportsTools` set per provider but never read in `buildRequestBody`. Native function calling never emitted. | `generic_openai.hpp:27`, `generic_openai.cpp:24-49` |
| PR08 | P1 | `listModels` hardcodes `isFree = false` regardless of API response. The `[free]` display path can never fire. | `generic_openai.cpp:306` |

### Manifest loader
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| ML12 | P0 | `cfg.fallbackProvider` and `cfg.fallbackModel` parsed but never read anywhere in runtime. Configured fallback does nothing. | `manifest_loader.hpp:104-105`, `types.hpp:122-123` |
| ML13 | P1 | `sandboxRuntime`/`sandboxImage` parsed but `launchDocker` hardcodes `ubuntu:24.04`. | `manifest_loader.hpp:132-133`, `sandbox_launcher.hpp:39` |
| ML14 | P1 | `AgentConfig::sandboxFiles` declared but never populated by `loadFiles` (vector returned but field not written). | `types.hpp:144`, `manifest_loader.hpp:314-332` |
| ML15 | P1 | `loadRelic` YAML parser never reads `endpoints:` or `health_path:` keys — fields stay at defaults. | `docker_dispatcher.hpp:62-79` |

### Relics
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| RL05 | P0 | `DockerRelicDispatcher::loadAllFrom()` has only one caller (its definition) — never invoked at runtime. All `managed`/`remote` relics permanently unroutable. | `docker_dispatcher.hpp:106` |
| RL06 | P0 | `DockerRelicDispatcher::dispatch` `remote` mode treats `endpoint` as full URL; callers pass a bare path string. No URL construction. CURL fails. | `docker_dispatcher.hpp:139` |

### Sandbox
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| SB06 | P0 | `SandboxPolicy::rewritePath()` exists but is never called from `dispatchTool`. Relative paths for fs/context tools are not rewritten to workspace-relative. | `policy.hpp:105-122` vs `agent_tools.cpp:40-53` |
| SB07 | P0 | `context_pin`/`peek`/`unpin` skip `validate()` entirely — sandboxed agents can pin arbitrary files (incl. host secrets). | `policy.hpp` |

### TUI / CLI
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| UI07 | P0 | `cortex-mk3 serve` subcommand is a stub: prints "use cortex-mk3-server binary instead" and exits 1. Documented surface unreachable. | `main.cpp:1186-1194` |
| UI08 | P1 | `RenderMode::SEMI` declared, has fallthrough `?` in `modeName`, never reachable from any CLI/REPL/server path. | `renderer.hpp:17,53` |
| UI09 | P1 | `ProtocolView::incremental()` and `resetStream()` only used by `tests/tui/pipe_demo.cpp` — REPL uses `render(width_)` instead. | `protocol.hpp:127-135` |
| UI10 | P2 | `ProtocolView` builtin fast-path set contains `"ethereal"` — typo for `ephemeral`/`context_peek`. Dead entry. | `protocol.hpp:215` |

### Server
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| SR01 | P0 | `/api/v1/agents` POST checks `body.isMember("max_tokens")` (snake) but reads `body["maxTokens"]` (camel) — caller's value silently discarded. | `server.cpp:68` |

### Session manager
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| SM01 | P1 | `SessionManager::prune()` implemented but never called — on-disk session files grow unbounded. | `manager.cpp:159-165` |
| SM02 | P2 | `prune()` skips `records[0]` (preserves earliest) — undocumented; means oldest records survive instead of newest. | `manager.cpp:164` |

### Utilities
| ID  | Sev | Summary | Source |
|-----|----|---------|--------|
| UT01 | P1 | `estimateTokens()` defined, zero callers. History capping is message-count only, never token-aware. | `token_estimate.hpp:17` |

---

## Suggested cut lines

**Sprint 1 — Foundations (everything halts on these):**
Y01, Y02, Y03, Y04, ML01, ML02, MA01, PP01, PP02, PP03, PP04, FE01, FE02, SB01, SB02, CL01, BT01

**Sprint 2 — Documented features made real:**
AC01, AC02, AC03, AC04, BT02, BT03, BT04, BT05, WF01, WF02, MF01, MF02, PR01, PR02, PR03, UI01

**Sprint 3 — QoL pass:**
The rest of P2, all P3 alongside nearby refactors.
