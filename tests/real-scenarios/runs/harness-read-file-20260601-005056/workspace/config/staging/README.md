# config/staging/

Modules under active development and testing. Each module is self-contained.

## Module Standard
Every manifest module follows this pattern:
```
<module-name>/
├── README.md      ← Documentation: purpose, usage, dependencies, promotion checklist
├── <manifest>.yml ← Manifest: kind, name, version, schema, imports
├── <script>       ← Script file (if runtime != builtin)
└── config.yml     ← Optional: per-module runtime config
```

## Lifecycle
```
config/staging/<type>/<module>/   ← Create & iterate
        │                               make test-*
        ▼
manifests/<type>/<module>/        ← Stable, promoted
        │
        ▼
examples/<type>/<module>/         ← Reference implementations (templates only)
```

## Current Module Types
| Type | Staging | Manifests | Examples |
|------|---------|-----------|----------|
| agents | assistant, decoupler | assistant, architect, builder... | c_agent, python_agent |
| feeds | — (WIP modules go here) | — | — |
| tools | — | — | — |
| relics | — | — | — |
| workflows | — | spec | — |
