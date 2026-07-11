# Inkcell Chat Business-Logic Parity Gate

**Status:** Gate implementation in progress; additive UI work is blocked until this document's required rows pass.  
**Oracle:** `src/tui/repl_session.hpp`  
**Replacement:** `src/ui/app`, `src/ui/chat`, `src/ui/model`, `src/ui/scenes`  
**Architecture invariant:** `src/ui` must not include `src/tui`.

## Required parity

| Capability | Inkcell implementation | Status |
|---|---|---|
| Blocking agent prompt runs off UI thread | `AgentBridge`, `runAgentTurn`, wake fd | PASS |
| Streaming protocol changes update in place | indexed `UiEvent`, protocol reducer | PASS |
| Partial responses do not create duplicate rows | indexed response upsert | PASS |
| Tool/sub-agent progress placeholders are not final results | progress suppression + final replacement | PASS |
| User/action/result/response transcript | `ShellModel` + `chat_view.hpp` | PASS |
| Long output wrapping and follow-bottom | `wrapTranscript`, bottom anchor | PASS |
| Honest run lifecycle and counters | per-turn reset/status model | PASS |
| Cancellation and next-turn recovery | Ctrl-C + `g_running` reset | PASS |
| Prompt editing and cursor | inkcell TextArea state + flat prompt rendering | PASS |
| Prompt history Up/Down | `ShellModel` history navigation | PASS |
| Prompt history persistence | `chat/prompt_history.hpp` | PASS |
| Tab command completion | dynamic + builtin command catalog | PASS |
| Session replay | structured `SessionRecord` mapping | PASS |
| Session continuation/save | core `Agent::prompt` / SessionManager path retained | PASS |
| `/help`, `/commands` | local command controller | PASS |
| `/manifests` | active config/capability counts | PASS |
| `/sessions` | SessionManager listing | PASS |
| `/prompts` | captured iteration prompt inspection | PASS |
| `/dump-prompt`, `/dp` | prompt export with status | PASS |
| `/cp-all`, `/cp-raw` | clipboard with local file fallback | PASS |
| `/clear`, `/thoughts`, `/raw` | local chat state actions | PASS |
| `/quit`, `/exit` | app quit route | PASS |
| Dynamic prompt/skill discovery | `chat_command_catalog.hpp` | PASS |
| Dynamic argument expansion | reviewed composer replacement | PASS |
| `ask_tool` blocking worker/UI handoff | AgentBridge ask channel | PASS |
| ask text/secret/textarea/number/key-value/type-confirm | extracted validation model + overlay | PASS |
| ask choice/confirm/multi-choice/ranker | keyboard controller + overlay | PASS |
| ask cancel and worker wakeup | bridge cancellation | PASS |
| Resize-safe rendering | inkcell Surface/App | PASS |
| Scroll/follow behavior | model offset + wrapped viewport | PASS |
| `src/ui` independence from `src/tui` | architecture smoke | PASS |

## Intentional omissions from the old TUI

These are not business-critical parity requirements:

- ANSI frame/debug dump (`/dump-render`, `/dr`). This inspects the implementation, not agent work.
- Legacy `SEMI` renderer mode. FULL, thoughts toggle, and RAW retain the useful data surfaces.
- Legacy custom frame clock and terminal diff engine. Inkcell owns frame scheduling/rendering.
- Legacy custom Input implementation and Emacs kill/yank/search bindings. Inkcell owns input primitives; core editing, cursor, history, persistence, and completion are retained.
- Persisted ANSI `rendered_history`. Inkcell replays structured `SessionRecord`s instead of coupling saved data to a renderer.

## Mandatory verification

```bash
make cortex-mk3
make test-ui-model
make test-ui-view
make test-chat-scene
tests/tui/ui_architecture_smoke.sh
tests/tui/experimental_chat_smoke.sh ./cortex-mk3
tests/tui/repl_parity_smoke.sh ./cortex-mk3
```

Live sub-agent acceptance:

```bash
MK3_TUI_SNAPSHOT=1 ./cortex-mk3 \
  -m manifests/agents/coder/agent.yml \
  --tui experimental --no-session \
  -p 'Ping reader once. Then report its reply in one sentence.'
```

Required result:

- one action row;
- no stream/status spam rows in FULL mode;
- no `reader is running…` fake result;
- completed reader result;
- complete response, not first chunk;
- status returns to done;
- pending count returns to zero.

No additive dashboard/main-menu work is allowed until this gate remains green.
