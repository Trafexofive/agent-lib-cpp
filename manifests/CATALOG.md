# Standard Manifest Catalog (PROD)

Last updated: 2026-07-21

## Scope

| Tree | Role |
|------|------|
| `manifests/` | PROD / std — hub recursive scan |
| `config/` | DEV / MVP — not auto-hubbed |

## Agents (5)

| Name | Path | Notes |
|------|------|-------|
| default | `agents/default/` | General agent |
| coder | `agents/coder/` | Coding coordinator |
| reader | `agents/coder/agents/reader/` | Nested specialist |
| tester | `agents/coder/agents/tester/` | Nested specialist |
| reviewer | `agents/coder/agents/reviewer/` | Nested specialist |

Archived from PROD (broken / empty): `config/agents/_archive/{brainstormer,std-orchestrator}`

## Built-in tools / feeds

See `built-in/tools/*`, `built-in/feeds/*`. Hub lists them as TOL/FED.

## Workflows

`workflows/*.yml` — inspectable in hub; full runtime renderer next.

## Hub

Dashboard → **Manifests** (`a`): recursive registry, `f` kind filter, Enter = launch hint (agents) or inspect notice (other kinds).
