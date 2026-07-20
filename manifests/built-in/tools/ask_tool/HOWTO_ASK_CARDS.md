# ask_tool / ask cards — operator + implementer notes

## Status (2026-03-27)

Inkcell TUI path is **live**, not vapor:

| Feature | Status |
|---------|--------|
| Modal overlay (`drawAskDialog`) | **shipped** |
| Card chain progress `card i/n · type` | **shipped** |
| Esc / Ctrl-C cancel → `bridge.cancelAsk` | **shipped** |
| Y/N confirm single key | **shipped** |
| Arrow / j k choice selection | **shipped** |
| Multi-select Space toggle | **shipped** |
| Number min/max, type_confirm exact word | **shipped** |
| Secret masked input | **shipped** |
| Default value on empty Enter | **shipped** (text/number/secret/textarea) |
| Worker unblock on answer (`completeAsk`) | **shipped** (P0 fix) |
| Notes-only auto-complete via `settleAsk` | **shipped** |
| Nested chains / branches / goto | **not yet** (pi ask_cards parity later) |
| optionsResolver / condition / transform | **not yet** |

## Call path

```
LLM <action type="tool" name="ask_tool">JSON</action>
  → Agent::dispatchAskTool
      → askToolHandler_  (inkcell: bridge.requestAsk)
          → UiEvent::AskDialog
          → ShellModel parseDialogState + overlay
          → operator keys → finishAskCard / settleAsk
          → bridge.completeAsk(results)  // unblocks worker
      → fallback: ToolRegistry native ask_tool (stdin) if no handler
```

## Supported card types (DialogState)

`text` · `textarea` · `secret` · `number` · `confirm` · `type_confirm` ·  
`choice` · `multi_choice` · `ranker` · `key_value` ·  
`note` · `info` · `section_header` (non-interactive, auto-advance)

Schema authority for the model: `manifests/built-in/tools/ask_tool/tool.yml`.

## Result shape

```json
{
  "success": true,
  "cancelled": false,
  "results": { "card_id": "value-or-array-or-bool" }
}
```

`success=false` + `cancelled=true` when operator Escapes. Do not re-ask the same chain.

## Tests

- `src/testing/ask_tool_test.cpp` — registry + stdin fallback
- `src/testing/ui_model_test.cpp` — bridge channel, number/type_confirm, settleAsk notes
- `src/testing/chat_scene_test.cpp` — choice roundtrip + notes-only auto-complete

## Next (parity backlog, not blockers)

1. condition / branches / goto between cards
2. optionsResolver (dynamic options)
3. transform pipeline (trim/lower/int)
4. minDelaySecs on destructive confirms
5. command_approval card type
