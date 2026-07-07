# diagram-junky

Playground for diagram data models, CLI renderer, MK3 manifest modules,
and a managed **diagram_workspace** relic that the TUI and the agent
share in real time.

## What's in here

| File / dir | What it is |
| --- | --- |
| `schemas/diagram-document.schema.json` | canonical document contract |
| `schemas/diagram-patch.schema.json` | patch envelope contract |
| `examples/*.diagram.json` | bundled sample diagrams |
| `render.py` | thin CLI over the renderer |
| `tui.py` | raw-ANSI TUI, local or client mode |
| `diagram_junky/rendering.py` | reusable logical-scene → character-canvas renderer |
| `diagram_junky/client.py` | `WorkspaceClient` + `EventStream` (SSE subscriber) |
| `manifests/relics/diagram-workspace/` | managed relic: server, Dockerfile, relic.yml |
| `manifests/tools/diagram-workspace/` | agent-callable tool over the relic API |
| `manifests/tools/diagram-render/` | agent-callable tool for local render/inspect/validate |
| `manifests/agents/diagram-junky/` | the agent that uses both |

## Tool ↔ Relic split

This is the day-to-day line that matters:

| Concern | Tool (what the agent calls) | Relic (what humans and other agents read) |
| --- | --- | --- |
| CRUD on workspaces, projects, diagrams | `diagram_workspace` (action: `workspace.create`, `diagram.put`, ...) | hosted on `http://localhost:8127` |
| Apply a small change | `diagram_workspace` (action: `patch.apply`) | `/patch/apply` |
| Render to ANSI text | `diagram_render` (local) **or** `diagram_workspace` (action: `diagram.render`, server-side) | `/diagram/render` |
| Validate schema | `diagram_render` (action: `validate`) | — |
| Live event log / SSE feed | `diagram_workspace` (action: `events.tail`) | `/events`, `/events/stream` |
| Active session pointer (for TUI follow-mode) | `diagram_workspace` (action: `session.set`) | `/session/active` |

The **tool** is what the agent calls. It speaks the relic API and
returns JSON envelopes. The **relic** is the long-running stateful
service. Every mutation appends an event that any client subscribed
to `/events/stream` can see in real time.

## Live test scenario

This is the loop the user can run today:

```bash
# 1. start the relic
docker compose -f playground/diagram-junky/manifests/relics/diagram-workspace/docker-compose.yml up -d
# (or: python3 playground/diagram-junky/manifests/relics/diagram-workspace/app/server.py)

# 2. attach the TUI as a client (real-time, follows the agent)
./playground/diagram-junky/tui.py --client --server-url http://localhost:8127

# 3. in another session, run the agent. Example prompt:
#    "Create a new workspace called 'brainstorm', add a project called
#     'core-ideas', and put a small flow diagram in it. Then add a
#     patch that adds an 'end' terminator connected to the existing
#     'work' node. Set the active session to that diagram when you're
#     done so I can see it in my TUI."
#
# 4. watch the TUI: the activity feed streams the agent's actions,
#    follow-mode (f) auto-opens the diagram the agent last touched,
#    live reload (L) re-renders as the agent patches the doc.
```

## CLI renderer

```bash
./playground/diagram-junky/render.py examples/minimal-flow.diagram.json --width 72 --height 12
./playground/diagram-junky/render.py examples/runtime-loop.diagram.json --width 150 --height 42
./playground/diagram-junky/render.py --example ansi-showcase --preset neon --ports

# QOL shortcuts
./playground/diagram-junky/render.py --examples
./playground/diagram-junky/render.py --styles
./playground/diagram-junky/render.py --example runtime-loop --inspect --validate
./playground/diagram-junky/render.py --example minimal-flow --output /tmp/minimal.txt
./playground/diagram-junky/render.py --example ansi-showcase --watch 0.5
```

## TUI

Two connection modes. **Local** (default) opens `.diagram.json` files
from disk; no server needed. **Client** (`--client`) attaches to the
relic, subscribes to `/events/stream`, follows the active session, and
auto-reloads the current diagram when it changes.

```bash
# local
./playground/diagram-junky/tui.py --example runtime-loop
./playground/diagram-junky/tui.py diagram.json

# client
./playground/diagram-junky/tui.py --client --server-url http://localhost:8127
./playground/diagram-junky/tui.py --client --example minimal-flow  # starts in canvas
```

### Keymap

| Key | Action |
| --- | --- |
| `j` `k` or arrows | select (dashboard) / pan (canvas) |
| `enter` `space` | open example / smooth center (canvas) |
| `0` | reset viewport |
| `f` | fit diagram (local) **or** toggle follow-active (client) |
| `L` | toggle live reload (client only) |
| `+` `-` | zoom in / out (relative to canvas center — the thing under the crosshair stays put) |
| `W` `P` `D` `R` | new workspace / new project / delete / rename (prompts for name) |
| `C` | copy current diagram into active project |
| `u` | refresh workspace state from server |
| `t` `c` `g` `o` `x` `b` | cycle theme, toggle color/legend/ports/crosshair/rail |
| `r` `s` | reload file / save session state |
| `:` | command palette |
| `/` | search diagrams |
| `?` | help overlay (any key dismisses) |
| `m` `esc` | harness chat overlay (model-backed, optional) |
| `a` | quick ask the harness pet about the current diagram |
| `q` | quit |

The TUI intentionally uses raw ANSI + stdlib terminal input. It is a
fast playground shell over the reusable renderer, not the final
hub/canvas app.

## Relic API surface

See `manifests/relics/diagram-workspace/relic.yml` for the canonical
endpoint catalog. The short version:

| Method | Path | Body | Purpose |
| --- | --- | --- | --- |
| GET | `/health` `/version` `/info` | — | liveness, version, server stats |
| GET | `/events?since=N&limit=M` | — | tail the event log |
| GET | `/events/stream` | — | server-sent events (text/event-stream) |
| GET | `/session/active` | — | read the active session pointer |
| POST | `/session/active` | `{workspace?, project?, diagram?, actor?}` | update the pointer (emits a `session.active` event) |
| POST | `/workspace/{create,rename,delete,list}` | varies | workspace CRUD |
| POST | `/project/{create,rename,delete,list,diagrams}` | varies | project CRUD |
| POST | `/diagram/{list,get,put,delete,render}` | varies | diagram CRUD + server-side render |
| POST | `/patch/apply` | `{id, ops, workspace?, project?}` | apply domain-aware patch ops |
| POST | `/lock/{acquire,release}` | `{resource, holder?}` | advisory locks with 30s TTL |

All mutations append a single event with a monotonic `seq`, the
actor, the workspace/project/diagram touched, a one-line summary, and
free-form `data`. The TUI subscribes to the stream and surfaces those
events in its activity feed.

## Document conventions

- `schema_version` must be `"diagram.document.v0"`.
- `id` matches `^[A-Za-z][A-Za-z0-9_.:-]{0,127}$`. Use kebab-case.
- `kind` is a hint: `flow`, `graph`, `state`, `sequence`,
  `architecture`, `freeform`, `unknown`.
- Position units are logical canvas units (floats), not pixels.
- Node `type` is semantic (`process`, `decision`, `actor`,
  `terminator`, `service`, `database`, `file`, `cloud`, `state`,
  `io`, `collection`, `external`, `note`, ...). The renderer maps
  types to shapes; override with `style.shape` if needed.
- Edges attach to nodes by `id`, optionally through a named `port`.
- Use `groups` for visual clustering and `annotations` for text notes.
- Renderers ignore unknown `data` and `style` fields — use `data` for
  metadata that may be useful later.

## Patch op vocabulary

`/patch/apply` takes a JSON list of ops:

```json
{"op": "node.add",       "node":     {...}}
{"op": "node.update",    "id": "...", "patch": {...}}
{"op": "node.remove",    "id": "..."}
{"op": "edge.add",       "edge":     {...}}
{"op": "edge.update",    "id": "...", "patch": {...}}
{"op": "edge.remove",    "id": "..."}
{"op": "annotation.add", "annotation": {...}}
{"op": "annotation.remove", "id": "..."}
{"op": "group.add",      "group":    {...}}
{"op": "group.remove",   "id": "..."}
{"op": "meta.set",       "patch":    {...}}
```

`node.remove` garbage-collects dangling edges. `meta.set` cannot
overwrite `nodes`, `edges`, `annotations`, `groups`, or
`schema_version` — use the dedicated add/update/remove ops for those.

## Renderer split

```
Diagram JSON -> logical scene -> styled character canvas
```

That split is intentional: the TUI keeps the same logical scene and
swaps the final canvas target later. The relic's `/diagram/render`
uses the same renderer so server-side previews and client-side renders
are byte-identical.

Current renderer tricks: solid/dashed/dotted/heavy/double edge lines,
box/diamond/cylinder node shapes, label halo pass, viewport overrides,
auto-pan/fit, port markers, debug legend, example resolver, preset
profiles, schema/docs helpers, file output, live reload loop.

## Running the smoke test

```bash
./playground/diagram-junky/smoke.sh
```

Three phases: local render + TUI, relic server end-to-end, tool
wrapper for every action. Exits 0 on success.

## Design stance

- **Document schema first.** The canonical object is a diagram
  document, not a renderer scene graph.
- **Canvas-neutral.** Coordinates are logical units. A terminal
  renderer, an SVG renderer, or a web canvas all map them later.
- **Node/edge core, everything else metadata.** Renderers can ignore
  unknown `data` and `style` fields.
- **Ports are first-class.** Edges attach to nodes or specific ports
  so tool/action graphs do not collapse into ambiguous arrows.
- **Patch-friendly.** The patch op vocabulary is JSON-Patch-ish but
  domain-aware (`node.add`, `edge.update`, etc.).
- **Tool and relic are distinct.** A tool is what the agent calls; a
  relic is what humans and other agents observe. The same HTTP API
  powers both, so the line stays clean.

## Non-goals for this prototype

- Multi-user semantics / CRDT
- Mouse interaction
- Web frontend
- Auth (this is a local managed relic; expose it on a trusted network only)
- Automatic layout (you place nodes by hand or with your own script)
