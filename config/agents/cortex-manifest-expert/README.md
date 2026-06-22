# cortex-manifest-expert

> A subject-matter expert sub-agent for MK3 manifests. Schema-first, audit-honest, test-driven.

## What it does

- Authors new tool, feed, relic, workflow, and agent manifests from a spec
- Reviews existing manifests for schema correctness and known pitfalls
- Audits manifest directories against the latest schema + audit findings
- Explains how a given manifest will behave at runtime
- Diagnoses manifests that load but don't behave as expected

## Quick start

Add to any agent's `import.agents:` list:

```yaml
import:
  agents: [default, cortex-manifest-expert]
```

Then from the model:

```xml
<action type="agent" name="cortex-manifest-expert" id="m1" mode="sync">
  Write a feed manifest with two tools (status, set) backed by Python scripts.
  Match the style of config/agents/morpheus/morpheus_dashboard/feed.yml.
</action>
```

## Philosophy

- **Schema first.** The model reads the parser, not the docs.
- **Minimal.** Smallest possible manifest that does the job. No gold-plating.
- **Audit-honest.** Findings are severity-tagged, with file:line evidence.
- **Test-driven.** A manifest without a working test is a hypothesis, not a deliverable.
- **Examples over explanation.** A working example is worth a paragraph.

## What it knows

- The full schema for tool, feed, relic, workflow, agent, harness, system, persona
- Common pitfalls (from the audit + live-test history)
- Runtime topology — how calls flow through dispatch
- The shared `process::run` substrate and its role in feeds/tools/relics/workflows
- The audit history (`docs/TICKETS.md`, `agent-lib-cpp-poc-stub-audit-*` artifacts)

See `references/` for the structured reference material. See `examples/` for working + broken manifests with annotations.

## Limitations

- The agent has read access only. It can write new manifests and example files, but cannot modify the runtime itself.
- It doesn't actually run the manifest against a real LLM in this mode. It can run `make test-feeds` / `make test-workflows` / `make test-relics` and inspect output, but full end-to-end validation requires the parent agent.
- The model is `deepseek-v4-pro` (same as Morpheus). If the user prefers a different model, edit `cognitive_engine.primary` in `agent.yml`.