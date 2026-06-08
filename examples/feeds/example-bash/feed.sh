#!/usr/bin/env bash
# =============================================================================
# example-bash feed — demonstrates Bash runtime feed pattern
# Outputs JSON with system load and disk usage
# =============================================================================
set -e

LOAD=$(uptime | awk -F'load average: ' '{print $2}' | cut -d',' -f1)
LOAD5=$(uptime | awk -F'load average: ' '{print $2}' | cut -d',' -f2)
DISK=$(df / | tail -1 | awk '{print $5}' | tr -d '%')
TIMESTAMP=$(date -Iseconds)

cat <<JSON
{
  "load_1m": $LOAD,
  "load_5m": $LOAD5,
  "disk_used_pct": $DISK,
  "timestamp": "$TIMESTAMP"
}
JSON
