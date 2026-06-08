# ci-runner

Runs test suites and reports pass/fail with evidence.

## Usage
```bash
./cortex-mk3 -m config/staging/agents/ci-runner/agent.yml
```
Then: `run make test and report results`

## Tools
- exec — run test commands
- list — find test files
- fs_read — read test output/logs
- grep — search for failures

## Module standard
- agent.yml — manifest
- config.yml — (none needed, uses defaults)
- system-prompts/ — persona
