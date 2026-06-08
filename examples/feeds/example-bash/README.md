# example-bash

**Runtime:** bash (shell)  
**Status:** 🧪 staging — template/example

## Purpose
Reference implementation for Bash-based feeds. Reports system load averages and root disk usage. Zero dependencies beyond coreutils.

## Output
```json
{"load_1m": 0.52, "load_5m": 0.43, "disk_used_pct": 72, "timestamp": "2026-05-14T17:30:00+00:00"}
```

## How to Create Your Own
```bash
cp -r config/staging/feeds/example-bash config/staging/feeds/my-shell-feed
# Edit feed.yml and feed.sh
# Test: make test-feeds
```

## Notes
- Uses `uptime`, `awk`, `df`, `date` — all standard Linux
- Output MUST be valid JSON on a single line
- Exit code non-zero → feed marked as failed

## Promotion Checklist
- [ ] Remove "example" from name
- [ ] Handle edge cases (disk full, load > 100)
- [ ] Move to `manifests/feeds/`
