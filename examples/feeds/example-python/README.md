# example-python

**Runtime:** python3  
**Status:** 🧪 staging — template/example, not yet promoted

## Purpose
Reference implementation for Python-based feeds. Reports timestamp, PID, and system uptime. Copy this module as a starting point for custom Python feeds.

## Output
```json
{"timestamp": "2026-05-14T17:30:00", "epoch": 1778769000, "pid": 12345, "uptime_seconds": 86400}
```

Rendered in prompt as:
```xml
<feeds>
  <example_python>
    timestamp: 2026-05-14T17:30:00
    epoch: 1778769000
    pid: 12345
    uptime_seconds: 86400
  </example_python>
</feeds>
```

## How to Create Your Own
```bash
cp -r config/staging/feeds/example-python config/staging/feeds/my-custom-feed
# Edit feed.yml — change name, summary, schema
# Edit feed.py — change logic
# Test: make test-feeds
```

## Dependencies
- Python 3 (standard library only)
- Linux `/proc/uptime` (optional — falls back gracefully)

## Promotion Checklist
- [ ] Remove "example" from name
- [ ] Production-quality error handling
- [ ] Move to `manifests/feeds/`
