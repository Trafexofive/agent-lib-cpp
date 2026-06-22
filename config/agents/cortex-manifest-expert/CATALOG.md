# cortex-manifest-expert

A self-contained manifest module: a sub-agent specialized in authoring, reviewing, and auditing MK3 manifests, with the reference material it needs in the box.

## When to use this agent

Reach for `cortex-manifest-expert` when the task is:

- **Authoring** a new tool, feed, relic, workflow, or agent manifest
- **Reviewing** an existing manifest for schema correctness
- **Auditing** a manifest directory against the latest schema + audit findings
- **Explaining** how a given manifest behaves at runtime
- **Diagnosing** a manifest that loads but doesn't behave as expected

The agent has read access to:

- `src/core/` (manifest loader, dispatch, agent runtime)
- `src/feeds/`, `src/relics/`, `src/tools/`, `src/workflows/` (per-kind sources)
- `manifests/` (stdlib manifests — reference)
- `examples/` (runtime templates)
- This module's `references/` and `examples/` directories

## How to invoke

In any agent manifest, add `cortex-manifest-expert` to the `import.agents:` list:

```yaml
import:
  agents: [default, cortex-manifest-expert]
```

Then from the model:

```xml
<action type="agent" name="cortex-manifest-expert" id="m1" mode="sync">
  Audit config/agents/morpheus/morpheus_dashboard/feed.yml against the latest schema.
</action>
```

## What's in this module

```
manifests/agents/cortex-manifest-expert/
├── agent.yml                       # agent definition
├── CATALOG.md                      # this file
├── README.md                       # quick-start + philosophy
├── system-prompts/
│   └── (inherits from ../../prompts/manifest-expert.md via context.system)
├── references/
│   ├── schema-tool.md              # tool manifest fields
│   ├── schema-feed.md              # feed manifest fields + tools: block
│   ├── schema-relic.md             # relic manifest fields
│   ├── schema-workflow.md          # workflow + step fields
│   ├── schema-agent.md             # agent manifest fields
│   ├── common-pitfalls.md          # audit findings + parser quirks
│   └── runtime-topology.md         # how calls flow through dispatch
└── examples/
    ├── good/                       # working manifests
    │   ├── tool-minimal.yml
    │   ├── tool-with-build.yml
    │   ├── feed-minimal.yml
    │   ├── feed-with-tools.yml
    │   ├── workflow-with-modifiers.yml
    │   ├── relic-minimal.yml
    │   └── agent-minimal.yml
    └── broken/                     # annotated wrong examples
        ├── feed-broken-empty-output.md
        ├── feed-broken-runtime.md
        ├── agent-broken-feed-path.md
        ├── workflow-broken-top-level-params.md
        └── tool-broken-runtime-mismatch.md
```

## Source of truth

The agent's system prompt points the model at the source files, not the docs. When in doubt, the model reads:

- `src/core/mini_yaml.hpp` — YAML parser
- `src/core/manifest_loader.hpp` — agent/relic/feed loader
- `src/core/dispatch.hpp` — tool/relic/agent dispatch
- `src/feeds/feed_engine.hpp` — feed manifest parser + dispatch
- `src/relics/docker_dispatcher.hpp` — relic def parser
- `src/relics/reliquary.hpp` — unified Relic registry
- `src/tools/tool.hpp` — tool def + execution
- `src/workflows/workflow_engine.hpp` — workflow execution

The reference docs are summaries, not the source. Always cross-check with the parser/dispatcher when something is unclear.

## Maintenance

The audit-driven schema additions in `docs/TICKETS.md` should be reflected here:

- New manifest fields → add to the relevant `schema-*.md`
- New pitfalls → add to `common-pitfalls.md`
- New dispatch paths → add to `runtime-topology.md`
- New good examples → add to `examples/good/`
- New bug patterns → add to `examples/broken/`

The agent itself is a normal sub-agent manifest. Edit `agent.yml` to change its tools, model, or imports.