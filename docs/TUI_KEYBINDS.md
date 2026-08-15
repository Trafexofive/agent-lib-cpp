# MK3 TUI Keybinds

Canonical keybinding reference for the `cortex-mk3` interactive TUI/REPL.

## Launching the TUI

```bash
cortex-mk3 run
cortex-mk3 run --repl
cortex-mk3 --continue run
cortex-mk3 --resume run
cortex-mk3 --session <session-id> run
```

If `run -p/--prompt` is used without `--repl`, MK3 runs one-shot mode instead of the interactive TUI.

## Prompt editing

| Key | Action |
|---|---|
| `Enter` | Submit current prompt |
| `\` then `Enter` | Insert newline / continue multi-line prompt |
| `Backspace`, `Ctrl-H` | Delete character before cursor |
| `Ctrl-D` | Exit if prompt is empty; otherwise delete character after cursor |
| `Ctrl-A` | Move cursor to start of line |
| `Ctrl-E` | Move cursor to end of line |
| `Ctrl-B`, `Left` | Move cursor left |
| `Ctrl-F`, `Right` | Move cursor right |
| `Alt-B`, `Ctrl-Left` | Move one word left |
| `Alt-F`, `Ctrl-Right` | Move one word right |
| `Ctrl-U` | Kill from cursor to start of line |
| `Ctrl-K` | Kill from cursor to end of line |
| `Ctrl-W` | Kill previous word |
| `Ctrl-Y` | Yank last killed text |
| `Tab` | Complete slash command prefix when prompt starts with `/` |

## History and search

| Key | Action |
|---|---|
| `Up`, `Ctrl-P` | Previous prompt history entry |
| `Down`, `Ctrl-N` | Next prompt history entry |
| `Ctrl-R` | Reverse-search prompt history |
| `Ctrl-R` again | Cycle search matches |
| `Enter` during search | Accept current search match and submit |
| `Esc`, `Ctrl-C`, `Ctrl-D` during search | Cancel search |

## Scrolling and redraw

### Inkcell agent chat (current)

Complementary binds — **no mode toggle**:

| Key | Action |
|---|---|
| **`j` / `k`** | Block select (history focus); viewport follows selection |
| **`Ctrl-J` / `Ctrl-K`** | Fine scroll transcript ±1 line (history **and** while typing) |
| **`gg` / `G`** | Select first / last block |
| **`Shift-[` / `Shift-]`** (or `{` / `}`) | Skip ±4 blocks |
| **`y`** | Yank selected block body (clipboard or `/tmp/mk3-yank.txt`) |
| `↑` / `↓` | Fine scroll ±1 (history focus) |
| `PageUp` / `PageDown` | Half-page scroll |
| `Home` / `End` | Jump transcript top / bottom |
| **`Ctrl-O`** | Toggle body truncate (forces full rewrap) |

Note: `Ctrl-J` is LF on classic TTYs; inkcell decodes it as `j`+Ctrl (distinct from Enter/`\r`).  
`Ctrl-K` in chat steals TextArea kill-to-EOL; `Ctrl-U` still kills-to-start.

### Legacy REPL notes

| Key | Action |
|---|---|
| `PageUp`, `Alt-K` | Scroll output/history up |
| `PageDown`, `Alt-J` | Scroll output/history down |
| `Ctrl-L` | Clear/redraw the screen |
| Terminal resize | Recalculates layout and redraws |

## Cancel / exit

| Key / command | Action |
|---|---|
| `Ctrl-C` | Cancel active prompt/input; during streaming, requests shutdown/cancel |
| `Esc` | Cancel active dialog; during streaming without dialog, cancels agent run |
| `Ctrl-D` on empty prompt | Exit REPL via `/exit` |
| `/exit`, `/quit` | Exit REPL |

## Dialog / ask_tool cards

When an `ask_tool` dialog is active, it takes over the viewport and some normal REPL keys are intercepted.

| Key | Action |
|---|---|
| `j`, `Down`, `Ctrl-N` | Move to next choice / ranker option |
| `k`, `Up`, `Ctrl-P` | Move to previous choice / ranker option |
| `y` / `Y` | Immediate yes for confirm cards |
| `n` / `N` | Immediate no for confirm cards |
| `Enter` | Submit text/current dialog line |
| `Esc` | Cancel dialog |

While a dialog is active, slash completion/search/scroll/clear-screen are blocked so the dialog owns input cleanly.

## Built-in slash commands

| Command | Action |
|---|---|
| `/help`, `/commands` | Show slash command catalog |
| `/manifests` | Show active tools, feeds, relics, agents, workflows |
| `/prompts` | Toggle prompt inspection mode |
| `/dump-prompt`, `/dp` | Write captured prompts to `/tmp/mk3-prompt-iterN.xml` |
| `/dump-render`, `/dr` | Write TUI render/debug state to the debug dump path |
| `/sessions` | List saved sessions |
| `/cp-all` | Copy visible history + output to clipboard, fallback `/tmp/mk3-cp-all.txt` |
| `/cp-raw` | Copy raw LLM output to clipboard, fallback `/tmp/mk3-cp-raw.txt` |
| `/exit`, `/quit` | Exit the REPL |

Additional prompt-library and skill slash commands may be loaded dynamically from manifests/config. Use `/commands` in the TUI for the complete live catalog.

## Related CLI session shortcuts

These are shell flags, not TUI keybinds:

| Command | Action |
|---|---|
| `cortex-mk3 -r run` / `cortex-mk3 --resume run` | Select a project-local session to resume |
| `cortex-mk3 -c run` / `cortex-mk3 --continue run` | Continue the most recently updated project-local session |
| `cortex-mk3 --session <id> run` | Use a specific session id |
| `cortex-mk3 --no-session run` | Ephemeral run; do not save session |

By default, non-ephemeral runs autosave under the current project’s `.cortex/sessions/` and state checkpoints under `.cortex/state/`. Resume reconstructs the runtime surface from session metadata before loading the checkpoint.
