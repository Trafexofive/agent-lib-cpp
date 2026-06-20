# Harness prompt versions

This directory holds candidate harness prompts for live testing before promotion to `manifests/harness/default.md`.

## Versions

- `default.v2026-06-18-runtime-hardened.md`
  - cloned from the dirty working `default.md` baseline, then rewritten for current runtime behavior.
  - Covers strict `final="true"` completion, empty-stream retry/surfacing, sub-agent `ephemeral` / `dump_context`, read-only results, and response-only sub-agent returns.

## Archives

See `../archive/`:

- `default.head-4877501.md` — committed HEAD baseline before this iteration.
- `default.worktree-pre-iteration.md` — exact dirty `default.md` at clone time.
- `default.old-preexisting.md` — pre-existing untracked `default.old.md` at clone time.

## Live testing

To test a candidate without promotion, point an agent manifest at it:

```yaml
context:
  harness: ../../../manifests/harness/versions/default.v2026-06-18-runtime-hardened.md
```

For `config/agents/morpheus/agent.yml`, from `config/agents/morpheus/`, the relative path is:

```yaml
context:
  harness: ../../../manifests/harness/versions/default.v2026-06-18-runtime-hardened.md
```

Promote only after live testing:

```bash
cp manifests/harness/versions/default.v2026-06-18-runtime-hardened.md manifests/harness/default.md
```
