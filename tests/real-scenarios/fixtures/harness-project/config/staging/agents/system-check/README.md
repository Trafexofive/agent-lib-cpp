# system-check

System environment verification agent. Checks tools, paths, disk space, network, and dependencies.

## Usage

```bash
./cortex-mk3 -m config/staging/agents/system-check/agent.yml
```

Then: `check if this environment is ready for development`

## Tools
- `exec` — run shell commands (which, --version, df, free, ping)
- `list` — check directory existence
- `fs_read` — read config files
- `grep` — search configs

## Module standard
- `agent.yml` — manifest
- `config.yml` — runtime config
- `system-prompts/` — persona
- No source code (uses built-in tools only)
