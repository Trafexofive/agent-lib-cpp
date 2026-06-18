# diagram_workspace relic

Starter managed relic for diagram-junky — stores diagram.document.v0 JSON by id.

## Running

```bash
docker compose -f playground/diagram-junky/manifests/relics/diagram-workspace/docker-compose.yml up -d
```

Or through cortex-mk3 with an agent manifest that imports this relic.
