# ask_tool

**Type:** tool  
**Runtime:** builtin (C++ — `internal_tools.cpp`)  
**Status:** ✅ stable

## Purpose
Agent presents structured dialog cards to the user and collects responses. Cards can be text input, confirmations, choices — any Pi TUI card type. The agent's action blocks until the user completes the dialog.

## Usage
```xml
<action type="tool" name="ask_tool" id="a1" mode="sync">
  {"title": "Confirm deployment", "message": "Deploy to production?", "cards": [
    {"id": "env", "type": "text", "title": "Target environment", "defaultValue": "staging"},
    {"id": "confirm", "type": "text", "title": "Type 'yes' to confirm"}
  ]}
</action>
```

## Parameters
| Param | Type | Description |
|-------|------|-------------|
| title | string | Top-level question title |
| message | string | Context message shown above cards |
| cards | array | Card chain (id, type, title, message, defaultValue per card) |

## Response
```json
{"success": true, "results": {"env": "staging", "confirm": "yes"}}
```

## Dependencies
- stdin/stdout (terminal only — not available in headless/sandbox mode)
