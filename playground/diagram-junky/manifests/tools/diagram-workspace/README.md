# diagram_workspace tool

MK3 tool wrapper around the `diagram_workspace` relic. The agent uses
this to CRUD workspaces, projects, and diagrams, apply patches,
render server-side, and read/set the active session pointer. Pairs
with the `diagram_workspace` managed relic manifest.

## Actions

| Action | Purpose |
| --- | --- |
| `info`, `health` | server stats and liveness |
| `workspace.list`, `workspace.create`, `workspace.rename`, `workspace.delete` | workspace CRUD |
| `project.list`, `project.create`, `project.rename`, `project.delete` | project CRUD |
| `diagram.list`, `diagram.get`, `diagram.put`, `diagram.delete` | diagram CRUD |
| `diagram.render` | render a document server-side (text-only) |
| `patch.apply` | apply domain-aware patch ops (`node.add`, `edge.update`, ...) |
| `session.get`, `session.set` | read or update the active session pointer |
| `events.tail` | read the event log since a given seq |

## Pairing with the render tool

Use `diagram_render` for pure local render/inspect/validate (no
server). Use `diagram_workspace` when the work needs to be visible to
the human operator and to other agents in the same session.
