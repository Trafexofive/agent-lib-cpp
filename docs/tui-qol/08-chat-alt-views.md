# Chat alt views (draft for operator discuss)

Primary remains the **timeline stream**. Alt views are projections, not rewrites of history.

## Candidates

| View | What it shows | When it wins |
|------|---------------|--------------|
| **Stream** (default) | Chronological blocks, kind chroma, rails | Daily drive, steer, cancel |
| **Graph** | Nodes = turns/tools/agents; edges = depends_on + parent→child | Multi-agent + fan-out; debug stuck wait |
| **Tree** | Nested agent wells as outline (no full drill) | Fleet glance without leaving parent |
| **Focus** | One block expanded (reader), siblings collapsed | Long tool dumps |

## Graph view sketch

- **Nodes**: YOU, thought (optional collapsed), ACTION(id), RESULT, RESPONSE, child Agent
- **Edges**: result←action, agent child←parent spawn, depends_on from protocol
- **Live**: pulsing node = current phase; rail color matches chat kind chroma
- **Nav**: hjkl / arrows on nodes; Enter drill child; Esc back to stream
- **Data**: project from `rootRows` + `protocolEvents` — no second source of truth

## Non-goals

- Not a full DAG editor
- Not pi canvas parity
- Not replacing nested drill composer

## Binding (proposal)

- `Ctrl-G` cycle Stream → Graph → Tree → Stream
- Footer pane dots stay Live/Session/Engine; view is orthogonal

Discuss before ship — colors/footer first.
