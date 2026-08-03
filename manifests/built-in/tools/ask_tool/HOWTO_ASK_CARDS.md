# ask_tool / ask cards — operator + implementer notes

## Status

Inkcell TUI path is **production** for coder daily-driver:

| Feature | Status |
|---------|--------|
| Modal overlay (`drawAskDialog`) | live |
| Card chain `card i/n · type` | live |
| Esc / Ctrl-C → `cancelAsk` | live |
| Y/N confirm single key | live |
| Arrow / j k choice | live |
| Multi-select Space toggle | live |
| Number min/max, type_confirm | live |
| Secret masked input | live |
| Default on empty Enter | live |
| Worker unblock (`completeAsk`) | live |
| Notes-only auto (`settleAsk`) | live |
| Result shape parity (answered/count) | live |
| Ask timeout (default 120s) | live |
| Headless without TTY | fails loud (no hang) |
| Nested chains / branches / goto | later (pi parity) |
| optionsResolver / condition | later |

## Call path

```
LLM <action type="tool" name="ask_tool" id="a1">JSON</action>
  → Agent::dispatchAskTool
      → askToolHandler_  (inkcell: bridge.requestAsk)
          → UiEvent::AskDialog
          → ShellModel parseDialogState + overlay
          → operator keys → finishAskCard / settleAsk
          → bridge.completeAsk(results)  // unblocks worker
      → else if TTY: stdin builtin fallback
      → else: error (no hang)
```

## Result shape (always)

```json
{
  "success": true,
  "cancelled": false,
  "timed_out": false,
  "results": { "card_id": "value" },
  "answered": ["card_id"],
  "count": 1
}
```

On Esc/cancel: `success=false`, `cancelled=true`, empty results.  
On timeout: `success=false`, `timed_out=true`, `error` set.

## Supported card types

`text` · `textarea` · `secret` · `number` · `confirm` · `type_confirm` ·  
`choice` · `multi_choice` · `ranker` · `key_value` ·  
`note` · `info` · `section_header`

Unknown types → treated as `text`. Empty/missing `cards` → one free-text card.

## Operator keys

| Type | Keys |
|------|------|
| text / number / secret / type_confirm | type · Enter submit · Esc cancel |
| confirm | y / n (no Enter) |
| choice | ↑↓ or j/k · Enter |
| multi_choice | ↑↓ · Space toggle · Enter |
| ranker | Enter accepts 1..n order, or type `3,1,2` |

## Timeout

Default **120s** waiting for operator (`requestAsk`). Override per call:

```json
{ "title": "…", "timeout_sec": 60, "cards": [ … ] }
```

`timeout_sec: 0` waits forever (Esc still cancels).
