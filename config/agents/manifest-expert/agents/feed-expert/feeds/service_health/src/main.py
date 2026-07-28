#!/usr/bin/env python3
# ./src/main.py
import json
import os
import sys
import time
import requests

BASE_URL = os.getenv("BASE_URL", "https://api.example.com")
TIMEOUT = int(os.getenv("TIMEOUT", "10"))
SERVICE_NAME = os.getenv("SERVICE_NAME", "api-gateway")


def main() -> int:
    url = f"{BASE_URL.rstrip('/')}/health"
    start = time.perf_counter()
    
    try:
        resp = requests.get(url, timeout=TIMEOUT)
        latency_ms = int((time.perf_counter() - start) * 1000)
        
        if resp.status_code == 200:
            data = resp.json()
            status = data.get("status", "healthy")
            output = {
                "ok": True,
                "service": SERVICE_NAME,
                "latency_ms": latency_ms,
                "status": status,
                "error": "",
                "timestamp": int(time.time())
            }
        else:
            output = {
                "ok": False,
                "service": SERVICE_NAME,
                "latency_ms": latency_ms,
                "status": f"http_{resp.status_code}",
                "error": f"http_{resp.status_code}",
                "timestamp": int(time.time())
            }
        
    except requests.exceptions.Timeout:
        output = {
            "ok": False,
            "service": SERVICE_NAME,
            "latency_ms": int(TIMEOUT * 1000),
            "status": "timeout",
            "error": "timeout",
            "timestamp": int(time.time())
        }
    except requests.exceptions.ConnectionError:
        output = {
            "ok": False,
            "service": SERVICE_NAME,
            "latency_ms": 0,
            "status": "unreachable",
            "error": "connection_failed",
            "timestamp": int(time.time())
        }
    except Exception as e:
        output = {
            "ok": False,
            "service": SERVICE_NAME,
            "latency_ms": 0,
            "status": "error",
            "error": str(e),
            "timestamp": int(time.time())
        }
    
    json.dump(output, sys.stdout)
    return 0 if output["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
