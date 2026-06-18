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
  ansi-showcase.diagram.json    # colors, shapes, line styles
  add-node.patch.json           # patch envelope example
render.py                       # CLI wrapper around reusable renderer core
tui.py                          # raw ANSI TUI playground prototype
diagram_junky/rendering.py      # reusable scene/canvas/CLI renderer core
```

## CLI renderer

```bash
./playground/diagram-junky/render.py \
  playground/diagram-junky/examples/minimal-flow.diagram.json \
  --width 72 --height 12

./playground/diagram-junky/render.py \
  playground/diagram-junky/examples/runtime-loop.diagram.json \
  --width 150 --height 42

./playground/diagram-junky/render.py \
  playground/diagram-junky/examples/ansi-showcase.diagram.json \
  --width 120 --height 36 --theme neon --color always --legend

# QOL shortcuts
./playground/diagram-junky/render.py --examples
./playground/diagram-junky/render.py --styles
./playground/diagram-junky/render.py --example ansi-showcase --preset neon --ports
./playground/diagram-junky/render.py --example runtime-loop --inspect --validate
./playground/diagram-junky/render.py --example minimal-flow --output /tmp/minimal.txt
./playground/diagram-junky/render.py --example ansi-showcase --watch 0.5
```

## Raw ANSI TUI prototype

```bash
./playground/diagram-junky/tui.py --example ansi-showcase
./playground/diagram-junky/tui.py --example runtime-loop --no-color
./playground/diagram-junky/tui.py --example minimal-flow --smoke-render
```

Keys:

- `h/j/k/l` or arrows: pan
- `+/-`: zoom
- `0`: reset viewport
- `f`: fit diagram bounds
- `n/p`: next/previous bundled example
- `t`: cycle theme
- `c`: toggle color
- `g`: toggle legend
- `o`: toggle port markers
- `r`: reload current file
- `s`: save viewport/session state to `~/.cache/diagram-junky/tui-state.json`
- `?`: show help
- `q`: quit

The TUI intentionally uses raw ANSI + stdlib terminal input. It is a fast
playground shell over the reusable renderer, not the final hub/canvas app.

Current renderer tricks:

- ANSI color policy: `--color auto|always|never`
- themes: `default`, `mono`, `neon`
- Unicode/ASCII modes
- square, rounded, double, heavy, dotted borders
- process/terminator/external/collection/actor defaults
- diamond decision nodes
- cylinder/database nodes
- solid, dashed, dotted, heavy, double edge lines
- selected/inverse node styling
- word-wrapped bodies + ellipsis fitting
- label halo pass so edges do not slice text
- viewport overrides: `--x`, `--y`, `--zoom`
- auto-pan/fit: `--fit`, `--fit-scale`, `--fit-upscale`, `--margin`
- optional port markers: `--ports`
- debug legend: `--legend`
- example resolver: `--examples`, `--example NAME`
- preset profiles: `--preset compact|wide|neon|poster`
- schema/docs helpers: `--validate`, `--inspect`, `--bounds`, `--styles`
- file output: `--output path`
- live reload loop: `--watch [seconds]`, `--no-clear`

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
