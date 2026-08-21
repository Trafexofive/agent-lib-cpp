# live-lab

Harness / manifest / TUI lab seat. Not a product agent.

**Engines:** `opencode-go/deepseek-v4-pro` → fallback `opencode-go/minimax-m3`  
**Children:** `echo-worker`, `probe-worker` (`opencode-go/deepseek-v4-flash`)

## Launch

Restart `cortex-mk3` after install, then:

```
cortex-mk3 -m live-lab
```

Offline graph check (no LLM):

```
./scripts/live-lab-smoke.sh
```

Live LLM (you must opt in):

```
CORTEX_LIVE=1 ./scripts/live-lab-smoke.sh
```

Operator script: `prompts/live-script.md`.
