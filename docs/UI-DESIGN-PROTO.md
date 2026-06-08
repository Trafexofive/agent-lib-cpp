# MK3 TUI — UI Design Prototype Document

Brainstorming all rendering aspects. User filters, I implement.

---

## 1. Screen Layout

```
┌─ Content Area (termH - 2 rows) ─────────────────────────┐
│                                                          │
│  [historyLines + current renderer output]                │
│  Scrollable via Ctrl-O/Ctrl-P / PgUp/PgDn                │
│                                                          │
├─ Status Bar (row termH-1) ───────────────────────────────┤
│ ⠼ ─── Mode: FULL · 45% ───                               │
├─ Input Line (row termH) ─────────────────────────────────┤
│ ▸ user types here█                                        │
└──────────────────────────────────────────────────────────┘
```

**Status bar shows:**
- Spinner `⠼` when LLM is running, hidden when idle
- Mode: FULL | RAW
- Scroll position: `· 45%` when content > viewport
- Compact, single line

---

## 2. FULL Mode — Action/Result Rendering

### 2.1 Action Block

```
  ⚙ exec#e1                                      ← dark blue-grey bg (#1e1e2e)
      command:  ls -la                            ← param: cyan bold key, dim value
      cwd:      /home/user
```

**Rules:**
- 2-space left margin (global indent)
- Row 1: dimmed icon + space + bold name + dimmed #id
- Following rows: 4-space indent, `key:` in cyan bold, value dimmed
- Background: `\033[48;2;30;30;40m` (dark blue-grey)
- Agent actions get orange icon/name

### 2.2 Result Block

```
  ✓ total 48                                      ← dark green-grey bg (#19281e)
    drwxr-xr-x  2 user user  4096 May 16 13:37 .  ← dimmed continuation lines
    drwxr-xr-x 15 user user  4096 May 13 21:32 ..

  ✗ failed — reviewer returned no output          ← dark red-grey bg (#28191e)
```

**Rules:**
- Green background for OK: `\033[48;2;25;35;30m`
- Red background for error: `\033[48;2;40;25;25m`
- Row 1: `✓` or `✗` + result text
- Following rows: 2-space indent continuation
- NO TRUNCATION — full output, always (user is debugging)

### 2.3 Builtin Tool — Custom Rendering

| Builtin | Rendering |
|---------|-----------|
| `exec` / `bash` | First line of output bold, rest dimmed |
| `list` / `ls` | `N entries` + first 3 entries indented |
| `read` / `fs_read` | First line dimmed |
| `write` / `edit` | `N chars` |
| `grep` / `search` | `N matches` |
| `pin` / `ethereal` | First line dimmed |

### 2.4 Non-Builtin Result

JSON output → styled YAML:
```
  ✓
    success:  true
    plan:
      - id:     1
        agent:  builder
        task:   Extract config module
```

Text output → show as-is with dimmed styling.

### 2.5 Block Separators

```
  ---                                             ← dimmed, between action+result pairs
```

Thin dimmed `---` separator line between each complete action+result block.

---

## 3. RAW Mode — Raw LLM Stream

```
Raw LLM output, dimmed, full width.
No sidebar. No hybrid pane layout.
System results interspersed inline as they arrive.

I'll list the directory.
<action type="tool" name="list" id="ls1" mode="sync">
{"path": "."}
</action>
<result id="ls1" status="ok">
{"entries": [{"name": "src"}, {"name": "README.md"}]}
</result>
The directory contains src/ and README.md.
<response final="true">
You have src/ and README.md.
</response>
```

**Rules:**
- Full raw LLM stream, dimmed
- `<action>` and `<result>` tags visible as-is (dimmed)
- No interpretation, no sidebar, no dual-pane
- User switches to FULL mode to see cleaned rendering

---

## 4. SEMI Mode — REMOVED

~~SEMI mode (split pane)~~ → removed. User said "no hybrid garbage."

Only two modes: FULL and RAW.
`/raw` toggles between them.
If third press: cycle back to FULL.

---

## 5. Response Rendering

```
── Response ──                                    ← dimmed separator

This is the **markdown** response text.           ← rendered by markdown component
- Lists render
- `code` renders inline

Final line of response.
```

**Rules:**
- `── Response ──` dimmed separator line
- Markdown rendered with full width - 4
- No box, no border
- Blank line after response before next prompt

---

## 6. Thought Rendering — REMOVED

~~`<thought>` blocks~~ → removed from harness. If LLM still emits them (old models), show dimmed or skip entirely in FULL mode.

---

## 7. Streaming Behavior

### 7.1 Current Problem
Full `\033[H\033[J` clear + redraw on every token causes visible flicker/stutter.

### 7.2 Proposed Fix — Cursor-Positioned Append

**During streaming:**
1. First frame: full `\033[H\033[J` clear + redraw everything
2. Subsequent frames: only rewrite changed lines. How:
   - Track `prevDisplay` (last rendered lines)
   - Find first changed line (usually the last few lines)
   - Move cursor there: `\033[{row};1H`
   - Clear and rewrite from that line down: `\033[2K` + new text + `\r\n`
   - Don't touch unchanged lines above

**When new content is only APPENDED** (streaming response grows):
- Move cursor to end of last rendered line
- Write new line: `\r\n\033[2K` + new content
- NO screen clear, NO flicker

### 7.3 Throttle

- Max 60fps during streaming (16ms between renders)
- Always render on first token (to show spinner → content transition)
- Always render on last token (to show final complete output)

### 7.4 Token Flow

```
LLM token arrives
  → parser.feed(token)
    → may emit ActionEvent → pv_.addAction()
    → may emit ResultEvent → pv_.addResult()
    → may emit ResponseToken → response_ += text
  → streaming callback fires
    → renderer.setRawStream(raw)
    → renderer.setResponse(response)
    → throttled renderScreen()
      → compute display = historyLines + renderer.render()
      → diff against prevDisplay
      → write only changed lines
      → update prevDisplay
```

---

## 8. Scroll / History

### 8.1 Behavior

- `historyLines` accumulates every previous turn (never cleared)
- Current turn output from renderer appended
- Viewport: `termH - 2` rows (leaving status bar + input line)
- `scrollOffset`: lines scrolled above viewport
  - 0 = bottom (latest)
  - N = N lines above viewport (older)

### 8.2 Key Bindings

| Key | Action |
|-----|--------|
| Ctrl-O | Scroll up (older content) |
| Ctrl-P | Scroll down (newer content) |
| PageUp | Scroll up one page |
| PageDown | Scroll down one page |
| Ctrl-N (readline) | Scroll down |

### 8.3 Scroll Reset

- On new prompt: scroll to bottom (`scrollOffset = 0`)
- During streaming: preserve scroll position (user can scroll back)
- On streaming end: don't reset (user stays where they were)

### 8.4 History Persistence

- `historyLines` survives across prompts
- Session saved to disk via SessionManager
- On restart: load session, rebuild historyLines from session records

---

## 9. User Prompt Visibility

### 9.1 Current Issue
User prompt `▸ Split test_data/monolith.py...` not visible in history during streaming.

### 9.2 Fix
- Add `▸ <user prompt>` to historyLines BEFORE streaming starts
- Visible immediately in history during response
- Displayed with bold styling

---

## 10. Status Bar

```
⠼ ─── Mode: FULL · 45% ───
```

**Components:**
- `⠼` — spinner (only when streaming=true)
- `─── Mode: FULL ───` — mode indicator (FULL or RAW)
- `· 45%` — scroll position (only when content > viewport)
- Dark/dim styling to not distract

**Spinner frames:** `⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏` (10 frames)
**Animation speed:** tied to renderScreen calls (~30fps during streaming)

---

## 11. Input Line

```
▸ user types here█
```

- `▸` bold prompt marker
- Text entry via Input component (readline or raw TUI)
- Cursor: block `█` style (reverse video on current char)
- Ctrl-A: home, Ctrl-E: end
- Up/Down in empty: scroll history

---

## 12. Color Palette

| Element | Hex | Usage |
|---------|-----|-------|
| bgAction | `#1E1E2E` | dark blue-grey | Action block background |
| bgResultOk | `#19281E` | dark green-grey | OK result background |
| bgResultErr | `#28191E` | dark red-grey | Error result background |
| Icon | `#3C3C50` | dim grey | Action icons (⚙ → ◇ ⏳) |
| Name | default bold | white | Tool/agent name |
| Key | `#32C8C8` | cyan bold | Param keys, YAML keys |
| Value | dim | grey | Param values, YAML values |
| StatusBar | dim | grey | Mode, scroll position |
| Spinner | `#FFC832` | orange-yellow | Loading indicator |
| Separator | dim | grey | `───` lines, `---` between blocks |
| AgentIcon | `#FFB400` | orange | `→` for agent actions |
| OK mark | `#00C800` | green | `✓` checkmark |
| Error mark | `#C83200` | red-orange | `✗` cross |
| UserPrompt | bold | white | `▸` user input marker |

---

## 13. What NOT To Do

- ❌ Box borders (`┌─┐│└─┘`) — user said no
- ❌ Dual-pane RAW mode — user said "no hybrid garbage"
- ❌ Truncation — user is debugging, needs full output
- ❌ SEMI mode — removed
- ❌ Thought blocks — removed from harness
- ❌ ANSI `\n` without `\r` — corrupts raw terminal display
- ❌ Resetting scroll during streaming — kills user's position

---

## 14. Implementation Order

1. Fix protocol.hpp → bg colors + separator + no truncation + full output
2. Fix renderer.hpp → remove SEMI, clean RAW mode
3. Fix main.cpp → cursor-positioned append for streaming (no full clear)
4. Fix harness → remove remaining `<thought>` references
5. Test with decoupler agent

---

## 15. Open Questions

- Collapsible results? (expand/collapse on keypress)
- Syntax highlighting in code output?
- Token count display in status bar?
- Dark/light theme toggle?
- `iterations.md` and `raw.md` dump — keep or remove?
