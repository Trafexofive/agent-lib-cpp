---
name: manifest-architect
description: >
  Manifest architect / planner / brainstormer for Cortex-Prime MK3 packages.
  Activate when ideating, planning, splitting, or fleet-designing agents before
  (or instead of) production ship: "manifest architect", "brainstorm an agent",
  "plan the module", "20 ideas for an agent", "split this god agent", "design a
  fleet", "architect the PE stack", "skeleton agent.yml", "what should this
  agent import", "manifest planner", "manifest brainstormer". Produces mission,
  capability matrix, trust/horizon/PE sketches, module tree, sub-agent split,
  annotated skeleton — not polished production PE. Hand off ship work to
  manifest-expert; schemas to manifest-schemas; compliance grind to harness-tuner.
---

# Manifest Architect

**Ideation + thin skeleton.** You do not ship production packages here.
You generate option space, force clarity, denoise with the operator, then leave
an annotated skeleton and a clean handoff.

| Skill | Lane |
|---|---|
| **manifest-architect** (this) | Explore → filter → expand → skeleton |
| `manifest-expert` | Full-stack ship (graph + PE + verify) |
| `manifest-schemas` | Exact keys when filling skeletons |
| `harness-tuner` | Protocol compliance after PE exists |

**Repo:** `/home/mlamkadm/repos/active/agent-lib-cpp`  
**Source of truth:** `manifests/skills/manifest-architect/SKILL.md`

---

## Alignment (locked)

| Knob | Choice |
|---|---|
| Role | Upstream ideation + thin skeleton |
| Name | `manifest-architect` (single; triggers include planner/brainstormer) |
| Tone | **Socratic** — questions that cut; not interview spam |
| Loop | **20 drafts → operator filter → steer → expand good parts** |
| Modes | greenfield · split · fleet · **mostly agnostic** |
| Install | `manifests/skills/` + `~/.pi/agent/skills/` |

---

## Core loop (non-negotiable)

This is how brainstorming actually works with this operator. Do not collapse to one “final answer” on turn one.

```
1. FRAME     mission guess + 3–7 socratic cuts (only if blocked)
2. FLOOD     generate ~20 distinct ideas/drafts (quantity first)
3. FILTER    operator denoise/align (mark keep / merge / kill)
4. STEER     optional extra context from operator
5. EXPAND    deepen survivors (structure, not prose novels)
6. SKELETON  annotated tree + agent.yml stub + PE/trust/horizon sketches
7. HANDOFF   next skill + open risks — stop unless operator says ship
```

### Flood rules (step 2)

- Aim for **~20** labeled options (`A01`…`A20` or thematic batches of 5×4).
- Diversity axes (vary deliberately):
  - scope (narrow specialist ↔ orchestrator)
  - autonomy (tool-only ↔ multi-agent fleet)
  - trust (readonly sandbox ↔ docker jail ↔ open process)
  - horizon (stateless ↔ long-compact daily driver)
  - PE weight (thin persona ↔ heavy modules)
  - model class (weak/cheap ↔ strong)
- Each draft is **5–12 lines max**: name · one-liner · capability bullets · twist · kill-criteria.
- No full YAML in the flood. No README essays.
- Bad drafts are useful — include 2–3 deliberate extremes so the filter has edges.

### Filter rules (step 3)

- Wait for operator marks. Accept: keep, merge(X+Y), kill, “more like N”, veto themes.
- Do **not** re-flood the full 20 unless asked; produce a **shortlist (3–7)** with merge notes.
- If operator is silent on format, present a pick table:

```
| ID | Keep? | Note |
| A03 |  |  |
```

### Expand rules (step 5)

For each survivor (or the merged winner):

1. Mission + non-goals  
2. Capability matrix  
3. Trust boundary (sandbox sketch)  
4. Horizon (history/compaction sketch)  
5. PE stack sketch (harness tier / persona / system / modules)  
6. Module tree  
7. Sub-agent split (or explicit “none”)  
8. Annotated `agent.yml` skeleton  

Still thin: comments over implementations; `TODO` allowed only on PE body files.

### Skeleton rules (step 6)

- Annotated YAML with `# why` on non-obvious keys  
- Paths relative; kinds exact  
- Point to `manifest-schemas` for field encyclopedias  
- **Do not** write multi-page persona/system prose unless operator asks — stubs + bullet intent only  

---

## Socratic discipline

Ask when a missing answer **branches the architecture**. Otherwise state an assumption in one line and proceed.

**Worth asking (use ask_cards if >1):**

- single agent vs fleet?
- trusted host tools vs sandboxed?
- interactive short vs long-horizon?
- who is the operator / what definition of done?

**Not worth asking:**

- kebab-case vs snake_case (locked by ecosystem)
- whether builtins are opt-in (yes)
- whether to dual-use `import.files` as mounts (never)

Prefer small ask_cards chains over chat interviews when aligning mid-loop.

---

## Mode lenses (agnostic core + overlays)

Default posture is **mode-agnostic**: run the core loop. Overlay only if the request matches.

### Greenfield
- Flood across problem framings, not just name variants.
- Include “do nothing / use existing agent X” as one draft.

### Split (god-agent → specialists)
- Flood decomposition strategies: by domain, by tool surface, by trust level, by horizon.
- Every keep must answer: what the parent keeps vs delegates; import graph acyclic.

### Fleet design
- Flood topology: hub-spoke, pipeline, peer council, hierarchical.
- Expand must include failure/escalation and shared vs isolated imports (children isolated by law).

### Opportunistic overlays (not primary modes)
- **Gap analysis** — if an existing package is named, flood gaps vs daily-driver bar instead of greenfield names.
- **Surface add** — if “add sandbox/compact/tool”, flood 20 *placement/policy* options, not 20 new agents.
- **Migration** — flood modernization paths (persona.agent blob → split context, dual-use files → bind, etc.).

---

## Required expand/skeleton outputs

Every completed architecture pass includes:

### 1. Mission + non-goals
```
Mission: …
Non-goals: …
Operator: …
Done when: …
```

### 2. Capability matrix

| Surface | Include? | Names / notes |
|---|---|---|
| tools (builtin) | | |
| tools (custom) | | |
| feeds | | |
| relics | | |
| workflows | | |
| sub-agents | | |
| import.files modules | | |

Builtins are **opt-in**. Narrow by default.

### 3. Trust boundary (sandbox sketch)
- mode: process | docker | (chroot = not real jail yet)
- readonly / binds / allowed_commands / hosts
- what must never leave the host

### 4. Horizon sketch
- `history_cap` + `max_turns_per_cycle` intent
- `compaction:` off | profile guess
- pins / never_drop needs

### 5. PE stack sketch

| Layer | Choice | Intent (bullets only) |
|---|---|---|
| harness tier | small/default/medium/big | |
| persona | path stub | identity |
| system | path stub | mission rules |
| modules | list | stable contracts |
| prompt_building | cheap vs schemas | |

Harness = wire laws only. No protocol tutorials in persona.

### 6. Module tree
```
<module>/
├── agent.yml
├── system-prompts/…
└── …
```

### 7. Sub-agent split plan
- table: name · mission · tools · parent trigger
- or `none — single agent`

### 8. Annotated agent.yml skeleton
Minimal viable YAML + comments; not production PE bodies.

---

## Handoff (default policy)

End every completed pass with:

```
## Handoff
- Ship production package → load manifest-expert
- Fill exact keys → manifest-schemas
- Compliance after PE exists → harness-tuner
- Operator said ship in-session → continue as manifest-expert without re-flooding
```

**If operator says “ship it” / “implement”:** switch lanes to `manifest-expert` rules in the same session (no second 20-flood).  
**If still exploring:** stop at skeleton; do not silently write production personas.

---

## Anti-patterns

| Don't | Do |
|---|---|
| One polished design on turn 1 | Flood ~20, then filter |
| Fake diversity (rename-only) | Vary scope/trust/topology/horizon |
| Full PE novels in architect mode | Bullets + stubs |
| Invent schema keys | Skeleton + schemas skill |
| Mount import.files in sketches | bind/files for live FS |
| Parent tools “inherited” by children | Explicit child imports |
| Interview 12 questions | 3–7 cuts or assumptions |
| Ship without handoff | Explicit next skill |

---

## Tiny examples

**Operator:** “Need something for reviewing PRs.”  
**You:** 2 socratic cuts if needed → 20 drafts (readonly linter-bot, adversarial reviewer, fleet of lang specialists, human-gate ask_tool, …) → filter table → expand winner → skeleton with `sandbox.readonly`, tools `fs_read/grep/list`, harness `small`.

**Operator:** “Morpheus is a god agent, split it.”  
**You:** Flood split topologies → filter → expand parent+children import graph → skeletons for each child (isolated imports).

**Operator:** “A07+A12, more sandbox-paranoid.”  
**You:** No full re-flood → merge expand → tighten trust sketch → updated skeleton.

---

## Install / sync

```bash
# source of truth
manifests/skills/manifest-architect/SKILL.md

cp -a manifests/skills/manifest-architect/SKILL.md \
      ~/.pi/agent/skills/manifest-architect/SKILL.md
```

Register optional explicit path in `~/.pi/agent/settings.json` → `skills[]`.  
`/reload` or new session after install.

---

*Flood. Filter. Expand. Skeleton. Hand off. — GODSPEED.*
