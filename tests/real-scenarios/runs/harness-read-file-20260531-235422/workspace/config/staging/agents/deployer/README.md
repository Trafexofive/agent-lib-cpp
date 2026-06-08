# deployer

Packages and deploys applications to target environments.

## Usage
```bash
./cortex-mk3 -m config/staging/agents/deployer/agent.yml
```
Then: `deploy to staging`

## Tools
- exec — docker, kubectl, rsync, scp, scripts
- list — verify artifacts
- fs_read — check configs
- fs_write — update configs
- grep — search logs

## Module standard
- agent.yml — manifest
- system-prompts/ — persona with safety checklist
- No source code (uses built-in tools)
