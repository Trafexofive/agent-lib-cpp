# security-expert — fleet module

Generalist security expert + orchestrator with five isolated specialists.
Triage, recon, breadth analysis in-hand; domain depth delegated; findings
synthesized into one severity-ranked, attributed report.

## Tree

```
config/agents/security-expert/
├── agent.yml                    # generalist orchestrator (hybrid hands)
├── system-prompts/              # persona + system (split PE)
├── prompts/                     # shared modules (import.files)
│   ├── finding-contract.md      # findings schema + severity rubric — all agents
│   ├── engagement-rules.md      # scope discipline + escalation contract — all agents
│   └── scope-checklist.md       # pre-engagement confirmation — parent
├── ctx/                         # shared engagement ledger (sandbox.bind → /workspace/ctx)
│   ├── engagement.md            # scope, authorization, decision log, findings
│   └── README.md
└── agents/                      # 5 isolated specialists (small harness)
    ├── binary_exploitation_expert/   # RE, memory corruption, exploit PoC
    ├── web_app_security_expert/      # HTTP/API surface, OWASP classes
    ├── network_security_expert/      # topology, exposure, TLS
    ├── osint_recon_expert/           # passive footprinting (no active ever)
    └── threat_intel_reader/          # advisories, CVEs, IOCs context
```

## Run

```bash
make cortex-mk3                        # if binary stale
./cortex-mk3 -m config/agents/security-expert            # parent (orchestrator)
./cortex-mk3 -m config/agents/security-expert/agents/binary_exploitation_expert   # standalone child
./cortex-mk3 --dry-run -m config/agents/security-expert  # load validation
```

## Trust model

Locked default + user-grant escalation. The sandbox is the mechanical floor
(`allowed_commands` / `allowed_hosts` / `readonly`); the real gate is the
engagement contract in `prompts/engagement-rules.md`, enforced by `ask_tool`
before any active interaction, out-of-scope write, or exploit attempt. The
operator is the ultimate authority. **Process mode does not jail the network** —
for hostile/untrusted targets, run under `sandbox.mode: docker` (deferred).

## Shared state

`ctx/` binds into every agent at `/workspace/ctx` — the only coupling between the
parent and its isolated children. Each child's `import:` is its own world; no
parent tool inheritance.

## Notes

- Model: xai/grok-4.5 everywhere (repo standard). Downgrade children per budget —
  the PE is thin enough that deepseek-class models can carry it.
- Offense tooling (nmap, sqlmap, gdb, pwntools-class) is listed in the relevant
  child's `allowed_commands` but executed only under operator approval. Missing
  tools fail honestly at exec time.
- Compaction: balanced + archive on parent and every child (long-horizon campaigns,
  deterministic standalone).

## Deferred (tracked)

- `sandbox.mode: docker` jail for hostile/untrusted targets.
- Findings relic (Postgres/SQLite) replacing the file ledger when campaigns scale.
- Workflow-routed dispatch (route.yml) if delegation patterns harden.
- crypto / malware / cloud / forensics specialists.
- Promotion to `manifests/agents/` (PROD hub) once it earns hub status.
