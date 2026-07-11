# Inkcell live test checklist — legacy parity path

Current truth:

- `--tui legacy` routes to extracted legacy `ReplSession`.
- `--tui inkcell` also routes to `ReplSession` as the parity/oracle path.
- `--tui experimental` launches the visible new inkcell app workbench.

## Interactive parity

Run both in a real TTY at the same terminal size:

```bash
./cortex-mk3 --tui legacy  --provider openai-codex --model gpt-5.5 --no-session
./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session
```

Expected: same product TUI surface:

- bottom-anchored transcript
- status bar on row H-1
- prompt line on row H
- same slash commands
- same scroll/cancel behavior
- same ask_tool dialog rendering
- same action/result cards
- same terminal restore after quit

## Non-live smoke already checked

```bash
(printf '/quit\r'; sleep 0.1) | \
  TERM=xterm COLUMNS=100 LINES=28 \
  ./cortex-mk3 --tui legacy --provider openai-codex --model gpt-5.5 --no-session

(printf '/quit\r'; sleep 0.1) | \
  TERM=xterm COLUMNS=100 LINES=28 \
  ./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session
```

Result: byte-identical ANSI output and stderr.

## Experimental app live test

Empty/start screen:

```bash
./cortex-mk3 -m manifests/agents/coder/agent.yml --tui experimental --no-session
```

One-shot visual timeline snapshot:

```bash
MK3_TUI_SNAPSHOT=1 ./cortex-mk3 --tui experimental --provider openai-codex --model gpt-5.5 --no-session -p 'Reply with exactly PONG.'
```

Expected now:

- `CORTEX MK3` topbar
- `history` section
- block-rendered stream/response/final rows
- visible `final` block containing `PONG`
- composer still present

## One-shot parity note

```bash
./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session -p 'Reply with exactly PONG.'
```

This intentionally uses the existing non-REPL one-shot path for now. The visible new app lane is `--tui experimental`.

## Next gates

1. Live TTY compare: legacy vs inkcell.
2. ask_tool dialog path under `--tui inkcell`.
3. Resize during streaming.
4. Cancel during provider wait and during tool execution.
5. Session resume replays rendered history.
