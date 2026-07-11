# Inkcell live test checklist

Run from a real TTY, not a pipe:

```bash
./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session -p 'Reply with exactly PONG.'
```

## Expected

- App opens in alt screen with outer inset, no edge-touching content.
- Header shows `CORTEX MK3`, value prop, mode chip, provider/model chip.
- Agent page is default.
- While running: status shows live/running, transcript shows stream byte progress.
- After completion: transcript contains final `PONG`.
- `2` routes to Dashboard.
- `3` routes to Inspector.
- `?` routes to Help.
- `1` returns to Agent.
- `q` or `Ctrl-C` quits and restores terminal.

## Review gates from sbtui spec

- No idle motion after run completes.
- No content touches terminal edge.
- Footer shows contextual keys and global status.
- Empty/loading/populated/error states are visible, not blank.
- No box spam: containment uses background panels/rules, not repeated cards.

## Known missing next

- Interactive composer/REPL is not wired yet. Requires input substrate work so scenes can receive unbound character keys/TextArea events cleanly.
- Command palette not wired yet.
- Manifest/session/provider scenes still legacy/planned.
