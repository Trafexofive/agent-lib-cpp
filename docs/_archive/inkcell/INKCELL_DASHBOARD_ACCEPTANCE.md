# Inkcell Dashboard Acceptance

**Status:** Implemented and gated.  
**Startup rule:** no manifest → Dashboard; explicit `-m/--manifest` → Chat.  
**Return rule:** from chat transcript focus, `m` → Dashboard.

## Required surfaces

- **Overview** — current agent, turn status, active session, manifest, capability counts.
- **Sessions** — real `SessionManager` inventory, keyboard selection, resume, clean-session creation, refresh.
- **Harness** — active manifest/harness/system/persona plus live tool/feed/relic/sub-agent names.
- **Runtime** — provider, model, theme, persistence mode, turn metrics. Provider/model are explicitly immutable for the active Agent instance; backend changes happen before launch.
- **Help** — complete dashboard navigation and actions.

## Required actions

| Action | Binding |
|---|---|
| Open chat | `c`, Overview `Enter` |
| Select section | arrows / `j` / `k`, or `o s h r ?` |
| Focus session list | `Tab`, Right, `s` |
| Resume selected session | Sessions `Enter` |
| Create clean session | Sessions `n` |
| Refresh sessions | `R` |
| Return to navigation | Left / Esc |
| Switch theme | `T` |
| Return from chat | transcript focus `m` |
| Quit | `q` |

## Non-negotiable behavior

- Session resume loads both the core `Agent` session and structured transcript records.
- New session clears Agent/chat history and switches future turns to the new session ID.
- No fake provider switch button. The existing Agent cannot safely swap providers in place.
- No main-page card grid or box spam.
- No “coming later” controls.
- No `src/ui` dependency on `src/tui`.
- Chat business-parity gate remains green.

## Verification

```bash
make test-ui-model
make test-chat-scene
tests/tui/experimental_chat_smoke.sh ./cortex-mk3
tests/tui/chat_business_parity_smoke.sh ./cortex-mk3
```

Snapshot expectations:

```bash
MK3_TUI_SNAPSHOT=1 ./cortex-mk3 --tui experimental --no-session
# Dashboard

MK3_TUI_SNAPSHOT=1 ./cortex-mk3 -m manifests/agents/coder/agent.yml --tui experimental --no-session
# Direct chat
```
