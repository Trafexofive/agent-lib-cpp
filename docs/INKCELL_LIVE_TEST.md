# Inkcell live test checklist

## Interactive REPL (real TTY)

```bash
./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session
```

Type a prompt, press **Enter** to send. Expect stream + final in timeline.

## One-shot (real TTY)

```bash
./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session -p 'Reply with exactly PONG.'
```

## Headless verification (already green)

```bash
MK3_TUI_SNAPSHOT=1 ./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session -p 'Reply with exactly PONG.'
```

## Keys

| Key | Action |
|-----|--------|
| Enter | send (composer focused) |
| Esc | focus timeline scroll |
| i | focus composer |
| 1/2/3/? | Agent / Dashboard / Inspector / Help |
| r | toggle raw stream lines |
| t | toggle thoughts |
| ↑↓ | scroll timeline (when unfocused) |
| q / Ctrl-C | quit |

## Review gates

- Outer inset, no edge-touch content
- No idle animation after turn completes
- Empty state before first prompt
- Loading/running chip during stream
- Final/response visible after turn
- Terminal restored after quit
