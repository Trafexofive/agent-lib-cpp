---
name: skill-creator
description: >
  Create a new pi agent skill from scratch. Use when the user asks to "create a skill for X",
  "write a SKILL.md for Y", "make a new skill", "build a skill module", "scaffold a skill",
  or any request to produce a new reusable skill file. Complements session-to-skill (which
  converts existing session history); this skill creates fresh skills from specifications.
  Produces complete SKILL.md files with required YAML frontmatter, activation rules, and
  executable instructions.
---

# Skill Creator

Create a new, production-quality pi agent SKILL.md from scratch.

## When This Skill Activates

Activate when the user asks to create a brand-new skill (not from session history — use
`session-to-skill` for that). Triggers include:

- "create a skill for X"
- "write a SKILL.md for Y"
- "make a new skill that does Z"
- "scaffold a skill module"
- "I need a skill for W"

## Required Frontmatter

Every SKILL.md must start with YAML frontmatter:

```yaml
---
name: kebab-case-name
description: >
  When to activate this skill. Include keyword triggers, task patterns, and scope.
  The description is what the agent reads to decide whether to load the skill.
---
```

## Skill Body Structure

After the frontmatter, structure the skill body with:

1. **Title** (`# Skill Name`) — clear, action-oriented
2. **When This Skill Activates** — refined trigger logic
3. **Core Instructions** — what the agent should do step-by-step
4. **Examples** — concrete input/output patterns
5. **Anti-Patterns** — what NOT to do

## Process

1. Gather the user's requirements — what should the skill do? When should it trigger?
2. Draft the frontmatter with precise description triggers
3. Write the body following the structure above
4. Save to `~/.pi/agent/skills/<name>/SKILL.md`
5. Validate: ask the user to `/reload` and confirm the skill loads without errors

## Validation

After writing, verify:
- [ ] YAML frontmatter is valid (no parse errors)
- [ ] `name` and `description` are present
- [ ] Description is specific enough to avoid false activation
- [ ] File is saved to the correct directory
