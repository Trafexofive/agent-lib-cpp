# MK3 Skill Library

Skills are reusable operating policies for MK3 agents, workflows, and the
Cortex/inkcell TUI stack.

| Skill | Purpose |
|-------|---------|
| `inkcell` | **Masterclass** — inkcell C++17 TUI framework (also global pi skill) |
| `mk3-manifest` | Cortex-Prime MK3 module/manifest conventions |
| `harness-tuner` | Harness prompt compliance, protocol enforcement, regression testing |

## Layout

```
manifests/skills/<name>/SKILL.md   # discovered as kind=skill by hub catalog
manifests/skills/INKCELL.SKILL.md  # root alias for inkcell masterclass
```

Pi-agent global copy (always loaded by pi discovery):

```
~/.pi/agent/skills/inkcell/SKILL.md
~/.pi/agent/skills/INKCELL.SKILL.md
```

Project pi copy (when repo trusted):

```
.pi/skills/inkcell/SKILL.md
```
