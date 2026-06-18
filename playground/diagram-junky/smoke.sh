#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - <<'PY' "$ROOT"
import json
import sys
from pathlib import Path
root = Path(sys.argv[1])
for path in sorted(root.rglob('*.json')):
    json.loads(path.read_text())
    print(f'json ok: {path.relative_to(root)}')
try:
    import jsonschema
except Exception as e:
    print(f'jsonschema unavailable: {e}')
    raise SystemExit(0)
doc_schema = json.loads((root/'schemas/diagram-document.schema.json').read_text())
patch_schema = json.loads((root/'schemas/diagram-patch.schema.json').read_text())
for path in sorted((root/'examples').glob('*.diagram.json')):
    jsonschema.Draft202012Validator(doc_schema).validate(json.loads(path.read_text()))
    print(f'document schema ok: {path.relative_to(root)}')
for path in sorted((root/'examples').glob('*.patch.json')):
    jsonschema.Draft202012Validator(patch_schema).validate(json.loads(path.read_text()))
    print(f'patch schema ok: {path.relative_to(root)}')
PY

"$ROOT/render.py" "$ROOT/examples/minimal-flow.diagram.json" --width 72 --height 12 >/tmp/diagram-junky-minimal.txt
"$ROOT/render.py" "$ROOT/examples/runtime-loop.diagram.json" --width 150 --height 42 >/tmp/diagram-junky-runtime.txt
"$ROOT/render.py" "$ROOT/examples/ansi-showcase.diagram.json" --width 120 --height 36 --theme neon --color always --legend --ports >/tmp/diagram-junky-ansi.txt
"$ROOT/render.py" --example ansi-showcase --width 100 --height 30 --theme neon --color never --fit --legend >/tmp/diagram-junky-fit.txt
"$ROOT/render.py" --example minimal-flow --preset compact --output /tmp/diagram-junky-output.txt
"$ROOT/render.py" --examples >/tmp/diagram-junky-examples.txt
"$ROOT/render.py" --styles >/tmp/diagram-junky-styles.txt
"$ROOT/render.py" --example ansi-showcase --inspect --bounds >/tmp/diagram-junky-inspect.txt
"$ROOT/render.py" --example ansi-showcase --validate >/tmp/diagram-junky-validate.txt

grep -q "Start" /tmp/diagram-junky-minimal.txt
grep -q "Do work" /tmp/diagram-junky-minimal.txt
grep -q "Protocol parser" /tmp/diagram-junky-runtime.txt
grep -q "Harness" /tmp/diagram-junky-ansi.txt
grep -q $'\033\\[' /tmp/diagram-junky-ansi.txt
grep -q "Minimal flow" /tmp/diagram-junky-examples.txt
grep -q "themes:" /tmp/diagram-junky-styles.txt
grep -q "bounds:" /tmp/diagram-junky-inspect.txt
grep -q "schema ok" /tmp/diagram-junky-validate.txt
grep -q "Start" /tmp/diagram-junky-output.txt

echo "render smoke ok"
