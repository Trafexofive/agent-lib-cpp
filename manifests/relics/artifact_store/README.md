# artifact_store

**Type:** relic  
**Runtime:** Docker (Python — FastAPI inside container)  
**Status:** 🧪 Docker runtime pending — dispatcher not yet wired

## Purpose
Persistent artifact storage for agent outputs. Agents can store, retrieve, list, and delete artifacts via relic endpoints.

## Endpoints
| Endpoint | Method | Description |
|----------|--------|-------------|
| `store` | PUT | Store an artifact with metadata |
| `retrieve` | GET | Retrieve by artifact ID |
| `list` | GET | List artifacts with optional tag filter |
| `delete` | DELETE | Remove an artifact |

## Schema
See `relic.yml` for endpoint definitions and schemas.

## Architecture
```
Agent → <action type="relic" name="artifact_store">
          {"endpoint": "store", "artifact_id": "...", "content": "..."}
       → RelicDispatcher → HTTP → Docker container (port 8100)
```

## Dependencies
- Docker runtime
- Python 3 + FastAPI + uvicorn (inside container)
- Container auto-started on first use, health-checked

## Promotion Checklist
- [ ] Docker relic dispatcher implemented and tested
- [ ] Health check endpoint verified
- [ ] Container lifecycle (start/stop/restart) robust
