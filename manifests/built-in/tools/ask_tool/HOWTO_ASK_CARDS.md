# How to Build ask_cards — A Proper TUI Dialog Widget

`ask_tool` is a minimal blocking stdin/stdout tool. `ask_cards` (future) will be a proper TUI dialog engine modeled after pi-agent's dialog system.

## Target: Full TUI Dialog Widget

### Architecture
```
Model (agent XML) → ask_tool (stderr/stdin bridge) → TUI layer → User
                                        ↓
                              ask_cards widget (future)
```

### Features to implement
| Feature | Status |
|---------|--------|
| Box-drawing borders around cards | TODO |
| Card chain progress `[1/3] [2/3]` | TODO |
| Default value display, Enter accepts | TODO |
| Esc to cancel entire card chain | TODO |
| Y/N confirm with single keypress (no Enter) | TODO |
| Arrow-key choice selection | TODO |
| Multi-select with Space to toggle | TODO |
| Input validation (regex, min/max, numeric) | TODO |
| Colored: title green bold, body dim, error red | TODO |
| Error message display below input | TODO |
| Markdown help text in cards | TODO |

### Implementation approach
1. Wake `src/tui/` module — terminal raw mode, ANSI cursor control, box drawing
2. Wake `src/tui/dialog.hpp` — card chain renderer using ansi.hpp primitives
3. `toolAskTool` delegates to `ask_cards::renderChain(params)` when GUI available
4. Use termios for raw mode (single-key Y/N, arrow keys)
5. Pi-agent reference: `/usr/lib/node_modules/@earendil-works/pi-coding-agent/docs/tui.md`

### Stack
- `src/utils/ansi.hpp` — ANSI escape codes (cursor, color, clear)
- `src/tui/terminal.hpp` — raw mode, screen buffer, key input
- `src/tui/dialog.hpp` — card rendering, input handling
- `src/tools/internal_tools.cpp` — `toolAskTool` entrypoint

### Current ask_tool (minimal)
```
stderr: [AGENT] What's your name?  ← no boxes, raw text
        > user types              ← blocks on getline
        [name] Your name           ← no progress indicator
        > Esc kills process        ← no cancel
```

### Target ask_cards (full widget)
```
┌─ [1/3] What's your name? ──────┐
│                                  │
│  Please tell me your name so     │
│  I can address you properly.     │
│                                  │
│  > ml______                      │
│                                  │
│  Esc to cancel                   │
└──────────────────────────────────┘
```

## API Contract

The full `ask_cards` widget takes the same JSON params as `ask_tool`:
```json
{
  "title": "Dialog title",
  "message": "Help text",
  "cards": [
    {"id": "name", "type": "text", "title": "Your name", "defaultValue": "user"},
    {"id": "confirm", "type": "confirm", "title": "Proceed?", "message": "This is irreversible"}
  ]
}
```

Returns: `{"success": true, "results": {"name": "user", "confirm": "yes"}}`
