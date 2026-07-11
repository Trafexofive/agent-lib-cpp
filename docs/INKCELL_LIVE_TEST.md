# Inkcell live test checklist

## Interactive

```bash
./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session
```

1. **Welcome** — only two options: Agent/History, Quit
2. Enter → **Agent/History**
3. Type prompt, Enter send
4. Esc → history focus
5. j/k select blocks
6. On `agent:… ↳ enter` block, Enter drills into that sub-agent history
7. Esc/Backspace pops back
8. Nested path shows in header breadcrumb
9. q / Ctrl-C quit, terminal restores

## One-shot (skips welcome)

```bash
./cortex-mk3 --tui inkcell --provider openai-codex --model gpt-5.5 --no-session -p 'Reply with exactly PONG.'
```

## Keys (Agent/History)

| Key | Mode | Action |
|-----|------|--------|
| Enter | composer | send |
| Esc | composer | history focus |
| i | history (root) | composer |
| j/k ↑↓ | history | select block |
| Enter | history | drill into sub-agent if drillable |
| Esc/Backspace/h | nested | pop history stack |
| g | nested | refresh sub-agent snapshot |
| r/t | history | raw / thoughts toggles |
| q | any | quit |

## Out of scope for now

- Dashboard / Inspector / Help pages (removed)
- Visual parity with legacy TUI (legacy still default / better for daily work)
- Full Action/Result card chrome
