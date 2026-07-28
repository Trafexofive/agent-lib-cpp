# Orchestra

Root control-plane **module**. Path-scoped under `config/`.  
Only `manifests/` is global bare-name scope.

```
config/agents/orchestra/
├── agent.yml              ← ROOT orchestrator (default entry)
├── system.md
├── persona.md
├── README.md
├── workflows/route.yml
├── agents/                ← local specialists (path-imported)
│   ├── planner/
│   ├── researcher/
│   └── skeptic/
├── helmsman/ forge/ lens/ archon/   ← personality variants
└── scenarios/
```

## Run

```bash
# root
./cortex-mk3 --provider xai --model grok-4.5 -m config/agents/orchestra -p "..."

# variants (same crew, different bias)
./cortex-mk3 --provider xai --model grok-4.5 -m config/agents/orchestra/helmsman -p "..."
./cortex-mk3 --provider xai --model grok-4.5 -m config/agents/orchestra/forge     -p "..."
./cortex-mk3 --provider xai --model grok-4.5 -m config/agents/orchestra/lens      -p "..."
./cortex-mk3 --provider xai --model grok-4.5 -m config/agents/orchestra/archon    -p "..."
```

## Import rule

```yaml
# root agent.yml
import:
  tools: [ask_tool]          # no hands
  agents:
    - ./agents/planner/agent.yml
    - ./agents/researcher/agent.yml
    - ./agents/skeptic/agent.yml
    - coder                  # manifests/ only bare name
```

| What | Reference |
|------|-----------|
| local specialist | path `./agents/<name>/agent.yml` |
| stdlib coder | bare `coder` |
| config peer (e.g. manifest expert) | path `../../cortex-manifest-expert/agent.yml` |

## Crew

| Agent | Model | Role |
|-------|-------|------|
| **orchestra** (root) | grok-4.5 | route / synthesize |
| planner | flash | decompose |
| researcher | flash | read-only facts |
| skeptic | flash | challenge plans/claims |
| coder | gpt-5.5 (+ flash tree) | implement & verify |

## Variants

| Path | Bias |
|------|------|
| `helmsman` | pure steady routing |
| `forge` | ship code via coder |
| `lens` | recon only (no coder) |
| `archon` | deliberate → skeptic → route |
