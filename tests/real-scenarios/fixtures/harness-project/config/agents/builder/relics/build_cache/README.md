# Build Cache Relic

Persistent cache for build artifacts. Avoids recompilation when source hasn't changed.

## Usage

```bash
docker compose up -d
```

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | /put | Cache an artifact (key + path) |
| GET | /get/{key} | Retrieve cached artifact info |
| DELETE | /invalidate/{key} | Remove from cache |
| GET | /health | Health check |
