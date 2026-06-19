# MK3 TUI Keybinds

Canonical keybinding reference for the `cortex-mk3` interactive TUI/REPL.

## Launching the TUI

```bash
cortex-mk3 run
cortex-mk3 run --repl
cortex-mk3 --continue run
cortex-mk3 --resume <session-id> run
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

| Key | Action |
|---|---|
| `PageUp`, `Ctrl-O` | Scroll output/history up |
| `PageDown` | Scroll output/history down |
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
| `cortex-mk3 -r` / `cortex-mk3 --sessions` | List saved sessions |
| `cortex-mk3 -c run` / `cortex-mk3 --continue run` | Continue the most recently updated session |
| `cortex-mk3 --resume <id> run` | Resume a specific session |
