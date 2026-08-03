# Harness prompts (operator notes — NOT injected)

These files are loaded into `<harness><protocol>…</protocol></harness>` in the
**system** message the model actually sees. They must be a **self-contained
surface of truth**.

## Rules

1. **No external pointers** — never "see CANON.md", never "docs/…". The model
   cannot open those files. If it is not in this text, it does not exist for the model.
2. **Protocol only** — tags, loop, action grammar, thought/result laws.
   Domain taste lives in agent `system.md` / `persona` / skills.
3. **Name the real prompt surface** — speak in terms of `<action_available>`,
   inline transcript, `<result>`, user continue cue — because that is what
   `buildSystemPrompt` / `buildChatPrompt` assemble every iteration.
4. **Tier by bytes, not essays**

| File | Inject when | Target |
|------|-------------|--------|
| `small.md` | specialists / children | ~1KB |
| `default.md` | daily parents / workers | ~2.5–3.5KB |
| `medium.md` | need agent/pipe nudge | ~3KB |
| `big.md` | debug / stubborn models only | ~5–6KB |

## What the model sees each iteration

```
system:
  <harness><protocol>{this file}</protocol><info …/></harness>
  <system>
    persona / system_prompt / skills / modules
    <action_available> tools relics feeds sub_agents workflows
    cwd + counts
    inline transcript (history capped)
  </system>
user:
  iter1 → real user request
  iterN → "Continue from the inline transcript…"
system (optional tail):
  dynamic feeds
```

Edit harness with that assembly in mind. Every line costs every generation.
