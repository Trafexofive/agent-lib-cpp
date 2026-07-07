#!/usr/bin/env bash
# Smoke test for the diagram-junky stack: schemas, renderer, TUI, relic, tool.
#
# Runs in three phases:
#   1. Local-only: JSON validity, schema validation, CLI renderer, TUI smoke
#   2. Relic server: starts the diagram_workspace server on a free port,
#      runs the full CRUD + patch + session flow over HTTP
#   3. Tool: invokes the diagram_workspace tool wrapper, asserts each
#      action returns success=true and the right shape
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

note() { printf "\033[36m· %s\033[0m\n" "$*"; }
ok()   { printf "\033[32m✓ %s\033[0m\n" "$*"; }
fail() { printf "\033[31m✗ %s\033[0m\n" "$*"; exit 1; }

PYTHONPATH="$ROOT${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH

# Pick a free port for the server (let the kernel choose, then read it back).
pick_free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(('127.0.0.1', 0))
print(s.getsockname()[1])
s.close()
PY
}

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "${DATA_DIR:-}" && -d "${DATA_DIR:-}" ]]; then
        rm -rf "$DATA_DIR"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Phase 1: local-only
# ---------------------------------------------------------------------------

note "phase 1: local-only checks (json, schemas, renderer, tui)"

python3 - <<'PY'
import json, sys
from pathlib import Path
root = Path(".")
for path in sorted(root.rglob('*.json')):
    if '__pycache__' in path.parts or 'node_modules' in path.parts:
        continue
    json.loads(path.read_text())
    print(f'  json ok: {path}')
try:
    import jsonschema
except Exception as e:
    print(f'  jsonschema unavailable: {e}')
    sys.exit(0)
doc_schema = json.loads((root/'schemas/diagram-document.schema.json').read_text())
patch_schema = json.loads((root/'schemas/diagram-patch.schema.json').read_text())
for path in sorted((root/'examples').glob('*.diagram.json')):
    jsonschema.Draft202012Validator(doc_schema).validate(json.loads(path.read_text()))
    print(f'  document schema ok: {path.relative_to(root)}')
for path in sorted((root/'examples').glob('*.patch.json')):
    jsonschema.Draft202012Validator(patch_schema).validate(json.loads(path.read_text()))
    print(f'  patch schema ok: {path.relative_to(root)}')
PY

"$ROOT/render.py" "$ROOT/examples/minimal-flow.diagram.json" --width 72 --height 12 >/tmp/dj-minimal.txt
"$ROOT/render.py" "$ROOT/examples/runtime-loop.diagram.json" --width 150 --height 42 >/tmp/dj-runtime.txt
"$ROOT/render.py" "$ROOT/examples/ansi-showcase.diagram.json" --width 120 --height 36 --theme neon --color always --legend --ports >/tmp/dj-ansi.txt
"$ROOT/render.py" --example ansi-showcase --width 100 --height 30 --theme neon --color never --fit --legend >/tmp/dj-fit.txt
"$ROOT/render.py" --example minimal-flow --preset compact --output /tmp/dj-output.txt
"$ROOT/render.py" --examples >/tmp/dj-examples.txt
"$ROOT/render.py" --styles >/tmp/dj-styles.txt
"$ROOT/render.py" --example ansi-showcase --inspect --bounds >/tmp/dj-inspect.txt
"$ROOT/render.py" --example ansi-showcase --validate >/tmp/dj-validate.txt
"$ROOT/tui.py" --example minimal-flow --no-color --smoke-render >/tmp/dj-tui.txt

grep -q "Start" /tmp/dj-minimal.txt
grep -q "Do work" /tmp/dj-minimal.txt
grep -q "Protocol parser" /tmp/dj-runtime.txt
grep -q "Harness" /tmp/dj-ansi.txt
grep -q $'\033\\[' /tmp/dj-ansi.txt
grep -q "Minimal flow" /tmp/dj-examples.txt
grep -q "themes:" /tmp/dj-styles.txt
grep -q "bounds:" /tmp/dj-inspect.txt
grep -q "schema ok" /tmp/dj-validate.txt
grep -q "Start" /tmp/dj-output.txt
grep -q "diagram-junky" /tmp/dj-tui.txt
grep -q "canvas" /tmp/dj-tui.txt
ok "phase 1: local render + tui ok"

# ---------------------------------------------------------------------------
# Phase 2: relic server end-to-end
# ---------------------------------------------------------------------------

note "phase 2: relic server end-to-end"

PORT="$(pick_free_port)"
DATA_DIR="$(mktemp -d -t dj-smoke-XXXXXX)"
export DIAGRAM_WORKSPACE_DATA="$DATA_DIR"
export DIAGRAM_WORKSPACE_PORT="$PORT"
python3 -B "$ROOT/manifests/relics/diagram-workspace/app/server.py" --port "$PORT" \
    >/tmp/dj-server.log 2>&1 &
SERVER_PID=$!

# Wait for the server to be reachable.
for _ in {1..50}; do
    if curl -s -m 1 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
curl -s -m 3 "http://127.0.0.1:${PORT}/health" | grep -q '"status":"healthy"'
ok "server up on :$PORT (pid $SERVER_PID)"

BASE="http://127.0.0.1:${PORT}"

# Create a workspace + project + diagram.
curl -s -m 3 -X POST -H 'Content-Type: application/json' \
    -d '{"workspace":"smoke","title":"smoke session"}' \
    "$BASE/workspace/create" | grep -q '"ok":true'
ok "workspace.create"

curl -s -m 3 -X POST -H 'Content-Type: application/json' \
    -d '{"workspace":"smoke","project":"main","title":"main project"}' \
    "$BASE/project/create" | grep -q '"ok":true'
ok "project.create"

curl -s -m 3 -X POST -H 'Content-Type: application/json' -d '{
  "workspace":"smoke","project":"main","id":"d1",
  "document":{
    "schema_version":"diagram.document.v0","id":"d1","title":"d1","kind":"flow",
    "nodes":[
      {"id":"a","type":"terminator","label":"start","position":{"x":0,"y":0},"size":{"w":10,"h":3}},
      {"id":"b","type":"process","label":"work","position":{"x":14,"y":0},"size":{"w":10,"h":3}}
    ],
    "edges":[{"id":"e1","type":"flow","source":{"node":"a"},"target":{"node":"b"}}]
  }
}' "$BASE/diagram/put" | grep -q '"ok":true'
ok "diagram.put"

curl -s -m 3 -X POST -H 'Content-Type: application/json' \
    -d '{"workspace":"smoke","project":"main"}' "$BASE/diagram/list" | grep -q '"d1"'
ok "diagram.list"

# Patch adds a node and an edge.
curl -s -m 3 -X POST -H 'Content-Type: application/json' -d '{
  "workspace":"smoke","project":"main","id":"d1",
  "ops":[
    {"op":"node.add","node":{"id":"c","type":"terminator","label":"end","position":{"x":28,"y":0},"size":{"w":10,"h":3}}},
    {"op":"edge.add","edge":{"id":"e2","type":"flow","source":{"node":"b"},"target":{"node":"c"}}}
  ]
}' "$BASE/patch/apply" | grep -q '"ops":2'
ok "patch.apply"

# Get the patched diagram and check the new node landed.
curl -s -m 3 -X POST -H 'Content-Type: application/json' \
    -d '{"workspace":"smoke","project":"main","id":"d1"}' "$BASE/diagram/get" \
    | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
doc = d.get("document", {})
ids = {n["id"] for n in doc.get("nodes", [])}
assert "c" in ids, f"node c missing in {ids}"
print("  patched diagram has node c")
'
ok "diagram.get reflects patch"

# Server-side render returns non-empty text.
curl -s -m 3 -X POST -H 'Content-Type: application/json' -d '{
  "width":60,"height":6,"theme":"neon","color":"never",
  "document":{
    "schema_version":"diagram.document.v0","id":"d1","title":"d1","kind":"flow",
    "nodes":[
      {"id":"a","type":"terminator","label":"start","position":{"x":0,"y":0},"size":{"w":10,"h":3}},
      {"id":"b","type":"process","label":"work","position":{"x":14,"y":0},"size":{"w":10,"h":3}}
    ],
    "edges":[{"id":"e1","type":"flow","source":{"node":"a"},"target":{"node":"b"}}]
  }
}' "$BASE/diagram/render" | grep -q '"rendered":"'
ok "diagram.render"

# Set + read the active session pointer.
curl -s -m 3 -X POST -H 'Content-Type: application/json' \
    -d '{"workspace":"smoke","project":"main","diagram":"d1"}' "$BASE/session/active" \
    | grep -q '"workspace":"smoke"'
ok "session.active set"

curl -s -m 3 "$BASE/session/active" | grep -q '"diagram":"d1"'
ok "session.active get"

# Event log has entries; tail and count.
EVENTS="$(curl -s -m 3 "$BASE/events?since=0&limit=200")"
echo "$EVENTS" | grep -q '"kind":"workspace.create"'
echo "$EVENTS" | grep -q '"kind":"project.create"'
echo "$EVENTS" | grep -q '"kind":"diagram.put"'
echo "$EVENTS" | grep -q '"kind":"diagram.patch"'
echo "$EVENTS" | grep -q '"kind":"session.active"'
ok "events.log has workspace/project/diagram/patch/session entries"

# SSE stream emits at least the hello frame within 1 second.
SSE="$(timeout 1 curl -sN "$BASE/events/stream" 2>/dev/null || true)"
echo "$SSE" | grep -q '"type":"hello"'
ok "events.stream SSE hello"

# Bad input -> 4xx with structured error.
ERR="$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d '{"workspace":"!!!"}' "$BASE/workspace/create")"
[[ "$ERR" == "400" ]] || fail "expected 400 for bad id, got $ERR"
ok "validation: bad id -> 400"

# Unknown endpoint -> 404.
NF="$(curl -s -m 3 -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d '{}' "$BASE/does/not/exist")"
[[ "$NF" == "404" ]] || fail "expected 404 for unknown endpoint, got $NF"
ok "validation: unknown endpoint -> 404"

# ---------------------------------------------------------------------------
# Phase 3: tool wrapper
# ---------------------------------------------------------------------------

note "phase 3: diagram_workspace tool wrapper"

TOOL="$ROOT/manifests/tools/diagram-workspace/src/main.py"

run_tool() {
    local body="$1"
    python3 -B "$TOOL" "$body"
}

for action in workspace.list workspace.create project.list project.create diagram.list diagram.get diagram.render session.get events.tail; do
    case "$action" in
        workspace.create)
            out="$(run_tool "{\"action\":\"workspace.create\",\"workspace\":\"tooltest\",\"title\":\"tooltest\",\"server_url\":\"$BASE\"}")"
            ;;
        project.create)
            out="$(run_tool "{\"action\":\"project.create\",\"workspace\":\"tooltest\",\"project\":\"one\",\"server_url\":\"$BASE\"}")"
            ;;
        project.list)
            out="$(run_tool "{\"action\":\"project.list\",\"workspace\":\"tooltest\",\"server_url\":\"$BASE\"}")"
            ;;
        diagram.list)
            out="$(run_tool "{\"action\":\"diagram.list\",\"workspace\":\"tooltest\",\"project\":\"one\",\"server_url\":\"$BASE\"}")"
            ;;
        diagram.get)
            out="$(run_tool "{\"action\":\"diagram.get\",\"workspace\":\"smoke\",\"project\":\"main\",\"id\":\"d1\",\"server_url\":\"$BASE\"}")"
            ;;
        diagram.render)
            out="$(run_tool "{
              \"action\":\"diagram.render\",\"width\":40,\"height\":6,\"theme\":\"neon\",\"color\":\"never\",
              \"server_url\":\"$BASE\",
              \"document\":{
                \"schema_version\":\"diagram.document.v0\",\"id\":\"d1\",\"title\":\"d1\",\"kind\":\"flow\",
                \"nodes\":[
                  {\"id\":\"a\",\"type\":\"terminator\",\"label\":\"start\",\"position\":{\"x\":0,\"y\":0},\"size\":{\"w\":10,\"h\":3}}
                ],
                \"edges\":[]
              }
            }")"
            ;;
        session.get)
            out="$(run_tool "{\"action\":\"session.get\",\"server_url\":\"$BASE\"}")"
            ;;
        events.tail)
            out="$(run_tool "{\"action\":\"events.tail\",\"since\":0,\"limit\":5,\"server_url\":\"$BASE\"}")"
            ;;
        *)
            out="$(run_tool "{\"action\":\"$action\",\"server_url\":\"$BASE\"}")"
            ;;
    esac
    echo "$out" | grep -q '"success":true' || fail "tool action $action failed: $out"
    ok "tool $action"
done

# Bad action -> structured failure (no exit code crash).
BAD="$(run_tool "{\"action\":\"nope\",\"server_url\":\"$BASE\"}" || true)"
echo "$BAD" | grep -q '"success":false'
ok "tool unknown action -> success:false"

# ---------------------------------------------------------------------------
# Cleanup is handled by the trap; the success line is the last thing printed.
# ---------------------------------------------------------------------------

ok "diagram-junky smoke ok"
