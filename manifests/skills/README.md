# MK3 Skill Library

Seed skills imported from `~/.pi/agent/skills/`.

A skill here is a reusable operating policy for a bespoke agent or workflow. The goal is not to dump every pi skill forever; the goal is to have a small, stable stdlib of prompts/policies that can be composed into MK3 agents.

## Current seed set

| Skill | Purpose |
|------|---------|
| `mk3-manifest` | Cortex-Prime MK3 module/manifest conventions |
| `harness-tuner` | Harness prompt compliance, protocol enforcement, regression testing |
| `test-driven` | Red/green/refactor discipline |
| `documentation` | Docs/README/API/changelog conventions |
| `git-workflow` | Safe git operations and commit hygiene |
| `session-to-skill` | Distill a session into a reusable skill |
| `skill-creator` | Create a fresh skill from a specification |
