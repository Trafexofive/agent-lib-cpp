# Finding contract — v1

Every finding produced by any agent in the security-expert fleet MUST be serializable
to this shape. Use it for the findings ledger (`ctx/findings.md`) and the final report.

## Shape

```json
{
  "id": "F-001",
  "title": "one line, root cause first",
  "severity": "critical|high|medium|low|info",
  "confidence": "confirmed|likely|possible|hypothesis",
  "category": "memory-corruption | xss | injection | authz | ssrf | exposure | misconfig | crypto | osint-leak | other",
  "target": "asset or endpoint",
  "cwe": "CWE-<id> when applicable",
  "cvss": 0.0,
  "evidence": ["verbatim tool output, code snippet, captured request"],
  "repro": "exact steps to reproduce, commands included",
  "impact": "what an attacker gains, concretely",
  "remediation": "one concrete change the operator can apply",
  "status": "open | accepted | mitigated | false_positive | fixed",
  "attribution": { "agent": "<specialist name>", "tools": ["<tool>"] },
  "references": ["URL/advisory"]
}
```

## Severity rubric (engineering judgment)

| severity | meaning |
|---|---|
| critical | remote, unauthenticated, or trivial-to-exploit full compromise of a scoped asset |
| high | significant compromise; requires auth or non-trivial conditions |
| medium | data exposure / partial control / defense bypass under conditions |
| low | hardening gaps, info leaks, best-practice violations |
| info | observations with no direct risk |

## Rules

- `severity` + `confidence` are mandatory and independent: a confirmed XSS is
  `confirmed`/`medium`; a suspicious pattern with no proof is `hypothesis`/`…` — never
  inflate confidence to justify severity.
- `evidence` must be verbatim output or exact code; paraphrase is not evidence.
- `repro` must be runnable by the operator.
- `attribution.agent` = the specialist that found it; the parent synthesizes, it does
  not claim credit.
- `id` increments per engagement ledger (F-001, F-002…).
