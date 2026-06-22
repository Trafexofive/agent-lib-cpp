# Relic Manifest Schema

**Authored at:** `manifests/relics/<name>/relic.yml` (or `poc/relics/<name>/relic.yml` for prototypes)

**Parsed by:** `src/relics/docker_dispatcher.hpp::loadDefFromDir` (flat fields), `src/relics/reliquary.hpp::loadDockerRelicsFrom`

**Loaded by:** `src/core/manifest_loader.hpp::loadRelics` (calls into the Reliquary now)

## Required fields

```yaml
kind: Relic
name: <snake_case>
version: "<semver>"
summary: "<one-line>"
```

## Mode: managed vs remote

A relic is either locally-managed (Docker compose stack) or remote (HTTP-only, no lifecycle).

```yaml
mode: managed                     # spin up via docker compose, route to localhost
mode: remote                      # endpoint IS a full URL; no lifecycle
```

## Managed relic fields

```yaml
port: 8100                        # port the service listens on inside the container
compose_file: "./docker-compose.yml"   # optional; defaults to docker-compose.yml
env_file: "./.env"                # optional; passed to docker compose
project_name: "my-project"        # optional; compose project name
health_path: "/health"            # optional; defaults to /health
```

If `port` is 0, the loader tries to auto-detect it from the `docker-compose.yml` `ports:` mapping (best-effort, regex-based).

## Common optional fields

```yaml
description: |                    # multi-line; used in <action_available>
  Long description of what the relic provides.
author: "<who>"
state: "stable"                    # stable, beta, experimental — informational
service_type: "security"           # free-form category
tags:
  - secrets
  - encryption
```

## Block fields (parsed but not used by runtime)

The current `DockerRelicDispatcher` only consumes flat fields. Block-style fields are read by `ManifestYaml` but ignored by the dispatcher — they exist for documentation and future use:

```yaml
interface:                        # ignored by current dispatcher
  type: "rest_api"
  base_url: "${SERVICE_URL:-http://localhost:8100}"

endpoints:                        # ignored by current dispatcher
  - name: "set"
    method: "POST"
    path: "/secrets/{ns}/{key}"
    parameters: { ... }

deployment:                       # ignored by current dispatcher
  type: "docker"
  docker_compose_file: "./docker-compose.yml"

health_check:                     # overrides `health_path` if present
  type: "api_request"
  endpoint: "/health"
  method: "GET"
  expected_status: 200
  interval_seconds: 30

environment:                      # ignored by current dispatcher
  variables:
    LOG_LEVEL: "WARNING"
```

If you need the block fields to take effect, you must extend `DockerRelicDef` and the parser. Until then, document them but expect the dispatcher to ignore them.

## Invocation syntax

```xml
<action type="relic" name="<relic>" id="r1" mode="sync">{"endpoint":"<endpoint>","key":"value"}</action>
```

The `endpoint` field tells the dispatcher which URL to hit. For remote relics, the `endpoint` is the full URL. For managed relics, the dispatcher prepends `http://localhost:<port>/`.

The body is forwarded as JSON to the relic's HTTP API.

## Lifecycle

For managed relics, the dispatcher:
1. Checks the health endpoint (`http://localhost:<port><health_path>`).
2. If unhealthy, runs `docker compose ... up -d` (via `process::run`).
3. Retries the health check up to 10 times at 500ms intervals.

`docker compose up` has a 120s timeout; longer startup times need an override.

## Examples

- `poc/relics/secret_store/relic.yml` — full block-style manifest (most fields ignored)
- `poc/relics/artifact_store/relic.yml` — flat-style manifest, runtime-consumed
- `playground/diagram-junky/manifests/relics/diagram-workspace/relic.yml` — minimal flat-style

## Common mistakes

1. **`mode: managed` without a `docker-compose.yml`** — the loader reports success (port defaults to 0 or auto-detected as 0), but `ensureContainerUp` will fail when invoked.
2. **Quoted `port: "8100"` vs unquoted `port: 8100`** — `ManifestYaml::get` returns the raw string; the parser does `std::stoi`. Both work but be consistent.
3. **Block-style fields expecting runtime behavior** — they don't. Only flat fields are consumed. The `interface:`, `endpoints:`, `deployment:` blocks are documentation-only today.
4. **Health check on a slow-starting container** — the 10-retry window is 5 seconds. If your container takes longer, the dispatcher will report a failed start. Bump `health_path` to a lightweight probe.
5. **Forget to import the relic in the agent** — same as feed: manifest parses, but no `<action type="relic">` handler is wired. Add to `import.relics:` or `import.feeds:` (wait, that's wrong — relics have their own list, see `src/core/manifest_loader.hpp`).