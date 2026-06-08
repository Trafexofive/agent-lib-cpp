#!/usr/bin/env python3
# =============================================================================
# example-python feed — demonstrates Python runtime feed pattern
# Outputs JSON with timestamp, PID, and system uptime
# =============================================================================
import json
import time
import os

data = {
    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
    "epoch": int(time.time()),
    "pid": os.getpid(),
}

# Read system uptime from /proc/uptime (Linux only)
try:
    with open("/proc/uptime") as f:
        uptime_secs = int(float(f.read().split()[0]))
        data["uptime_seconds"] = uptime_secs
except Exception:
    data["uptime_seconds"] = -1

print(json.dumps(data))
