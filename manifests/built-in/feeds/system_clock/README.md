# system_clock

**Type:** feed  
**Runtime:** builtin (C++ — `feed_engine.hpp`)  
**Status:** ✅ stable

## Purpose
Reports current system time in multiple formats — human-readable, ISO 8601, and Unix epoch.

## Output
```
Current time: Thursday, May 14 2026 15:30:00 (ISO: 2026-05-14T15:30:00.123456)
Unix: 1778769000 | Date: 2026-05-14 | Time: 15:30:00
```

## Promotion
Originally staged in `config/staging/feeds/system-clock/`. Promoted to `manifests/built-in/feeds/system_clock/` after stabilization.

## Usage
Registered by default. No import needed — builtin feeds auto-load.
