# diagram-junky

Playground for diagram data models before touching the TUI canvas or frontend/backend boundary.

## Goal

Define a small, durable diagram interchange format that can support:

- freeform boxes/arrows on an infinite-ish canvas
- flow/graph diagrams with typed ports
- future TUI rendering without baking terminal layout into the data model
- future frontend/backend sync via patches/events
- model-generated diagrams that are easy to validate and repair

## Files

```text
schemas/
  diagram-document.schema.json  # persisted diagram document
  diagram-patch.schema.json     # mutation envelope for UI/backend sync later
examples/
  minimal-flow.diagram.json     # tiny valid flow diagram
  runtime-loop.diagram.json     # MK3-ish harness/runtime diagram
  add-node.patch.json           # patch envelope example
render.py                       # first CLI renderer prototype
```

## CLI renderer

```bash
./playground/diagram-junky/render.py \
  playground/diagram-junky/examples/minimal-flow.diagram.json \
  --width 72 --height 12

./playground/diagram-junky/render.py \
  playground/diagram-junky/examples/runtime-loop.diagram.json \
  --width 150 --height 42
```

Renderer split:

```text
Diagram JSON -> logical scene -> character canvas
```

That split is intentional: the TUI should keep the same logical renderer and swap the final canvas target later.

## Design stance

- **Document schema first.** The canonical object is a diagram document, not a renderer scene graph.
- **Canvas-neutral.** Coordinates are logical units. A terminal renderer, SVG renderer, or web canvas maps them later.
- **Node/edge core, everything else metadata.** Renderers can ignore unknown `data` and `style` fields.
- **Ports are first-class.** Edges attach to nodes or specific ports, so tool/action graphs do not collapse into ambiguous arrows.
- **Patch-friendly.** The patch schema is JSON-Patch-ish but domain-aware (`node.add`, `edge.update`, etc.).

## Non-goals for this prototype

- TUI rendering
- mouse/key interaction model
- persistence backend
- CRDT/multi-user semantics
- automatic layout

Those come after the document and patch contracts feel right.
