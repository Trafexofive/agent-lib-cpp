#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MANIFEST="config/staging/agents/deepsearch-stack/agent.yml"

./cortex-mk3 --manifest "$MANIFEST" --dry-run >/tmp/dss-module-dry-run.out

python3 config/staging/agents/deepsearch-stack/tools/dss_health/src/main.py '{"checks":["health","warehouse_stats"]}' >/tmp/dss-health-tool.json
python3 -m json.tool /tmp/dss-health-tool.json >/dev/null

python3 config/staging/agents/deepsearch-stack/tools/dss_search/src/main.py '{"query":"rust memory safety","providers":["wikipedia"],"max_results":1}' >/tmp/dss-search-tool.json
python3 -m json.tool /tmp/dss-search-tool.json >/dev/null

python3 - <<'PY'
import json
import subprocess
search_result = json.load(open('/tmp/dss-search-tool.json'))
params = {'query': 'rust memory safety', 'search_result': search_result}
out = subprocess.check_output([
    'python3',
    'config/staging/agents/deepsearch-stack/tools/dss_synthesize/src/main.py',
    json.dumps(params),
], text=True)
result = json.loads(out)
assert result.get('success') is True, result
assert result.get('source_count', 0) >= 1, result
PY

python3 config/staging/agents/deepsearch-stack/tools/dss_search/src/main.py '{"query":"agentic RAG architecture","mode":"search","max_results":1}' >/tmp/dss-search-gateway-tool.json
python3 -m json.tool /tmp/dss-search-gateway-tool.json >/dev/null

python3 config/staging/agents/deepsearch-stack/tools/dss_warehouse/src/main.py '{"operation":"search","query":"rust","limit":1}' >/tmp/dss-warehouse-tool.json
python3 -m json.tool /tmp/dss-warehouse-tool.json >/dev/null

python3 config/staging/agents/deepsearch-stack/feeds/dss_health/src/main.py >/tmp/dss-health-feed.json
python3 -m json.tool /tmp/dss-health-feed.json >/dev/null

python3 config/staging/agents/deepsearch-stack/tools/research_strategy/test_strategy.py

printf 'deepsearch-stack manifest/tool/feed smoke passed\n'
