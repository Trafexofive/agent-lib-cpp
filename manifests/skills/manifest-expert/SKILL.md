---
name: manifest-expert
description: >
  Full-stack Manifest Expert for Cortex-Prime MK3 / agent-lib-cpp: YAML manifests
  (Agent/Tool/Feed/Relic/Workflow), portable modules, sandbox, compaction, import
  graph — PLUS meta-prompt / harness engineering (prompt assembly stack, CANON
  protocol, harness tiers, persona vs system vs import.files, prompt_building,
  compliance diagnosis). Activate for: agent.yml, tool.yml, feed.yml, relic.yml,
  workflow.yml, "manifest expert", "scaffold an agent", "portable module",
  sandbox.bind, compaction profile, import.files vs sandbox.files, harness.md,
  persona prompt, system prompt, "why is the model bare-texting", protocol
  compliance, prompt_building, history_cap, wire sub-agents, cortex-manifest-expert,
  "design the PE stack", "which harness tier". Prefer this skill when shipping a
  working agent package end-to-end. Load manifest-schemas for exhaustive schema dumps;
  load harness-tuner alone only for pure compliance iteration on an existing harness.
---

# Manifest Expert — Full Stack

You design **working agent packages**, not YAML in isolation.

A shippable MK3 agent is three coupled systems:

```
┌─────────────────────────────────────────────────────────────┐
│ 1. MANIFEST GRAPH     agent.yml + imports + sandbox/compact │
│ 2. PROMPT ASSEMBLY    harness · persona · system · modules  │
│ 3. RUNTIME CONTRACT   CANON protocol · parser · loops       │
└─────────────────────────────────────────────────────────────┘
```

If you only write `agent.yml` and dump a chatbot persona into `persona.agent`,
you shipped a demo. If you only tune the harness and ignore imports/sandbox,
you shipped a fragile specialist. **Do both.**

**Repo:** `/home/mlamkadm/repos/active/agent-lib-cpp`  
**Skill source of truth:** `manifests/skills/manifest-expert/SKILL.md`  
**Promote to:** `.pi/skills/manifest-expert/` · `~/.pi/agent/skills/manifest-expert/`

---

## 0. Skill routing (don't thrash)

| Need | Skill |
|---|---|
| Ideation / 20-draft flood / split-fleet planning / skeleton | `manifest-architect` |
| Full agent package / review / ship | **this skill** |
| Exhaustive per-key schema encyclopedia | `manifest-schemas` |
| Pure harness compliance iteration (bare text, tags) | `harness-tuner` (or §4 here) |
| Protocol law vs code | `docs/protocol/CANON.md` |
| Inkcell TUI | `inkcell` |

When the shape is unknown: **manifest-architect** first. When shipping a known shape: **start here**.

---

## 1. Operating protocol

1. **Name the deliverable** — new module · patch · review · compliance fix · surface (sandbox/compact).
2. **Map the stack** — which layers change? (manifest / harness / persona / system / modules / runtime knobs)
3. **Read before write** — existing YAML, persona, harness tier, CANON § if protocol-adjacent.
4. **Design top-down** — purpose → capability surface → PE stack → YAML → verify.
5. **Narrow by default** — few tools, small harness, cheap `prompt_building`, explicit sandbox.
6. **Verify two ways** — load (`--dry-run`) **and** behavior (protocol/compliance when PE changed).
7. **Rebuild if C++ touched** — stale `cortex-mk3` lies in dry-run.

### Deliverable shape

**Authoring:** module tree · YAML · PE files · verify commands+results · open risks  
**Review:** PASS/PARTIAL/FAIL per layer (graph · PE · sandbox · compact · protocol fit) · one-line findings · minimal patch

---

## 2. Prompt assembly stack (meta-prompt architecture)

### 2.1 What the model actually sees

Every generation (simplified):

```
system:
  <harness>
    <protocol>… harness markdown …</protocol>     ← wire laws ONLY
  </harness>
  <system>
    <persona>…</persona>                            ← identity / values / operator relationship
    <agent>… system prompt …</agent>                ← domain rules, mission, non-protocol policy
    <module name="…">…</module>                     ← import.files (static)
    <action_available>… tools/relics/agents …</action_available>
    … cwd, caps …
  </system>
  [inline execution transcript — history_cap / compaction apply here]

user:
  <user>…</user> | continue cue
```

Code: `Agent::buildSystemPrompt` · `src/core/agent_prompts.cpp`  
Harness load: constructor pre-indents `config.harnessPath`  
Persona: `config.personaPath`  
System/agent body: `config.systemPromptPath` / legacy `persona.agent`  
Modules: `import.files` → `__PROMPT_MODULES_XML__`

### 2.2 Layer responsibilities (non-negotiable)

| Layer | File / key | Contains | Must NOT contain |
|---|---|---|---|
| **CANON** | `docs/protocol/CANON.md` | Law for humans + code | Model PE theater |
| **Harness** | `context.harness` / default `manifests/harness/*.md` | Tags, attrs, loop, pipe, agent ops | Roleplay, domain taste, “you are not a chatbot”, doc pointers |
| **Persona** | `persona.agent` or `context.persona` | Who the agent is, values, operator relationship, tone | Protocol tag tutorials (belong in harness) |
| **System** | `context.system` / system prompt path | Mission, domain rules, workflow policy, quality bar | Duplicate harness wire laws |
| **Modules** | `import.files` | Stable contracts, checklists, schemas-as-prose | Live FS mounts |
| **Capability XML** | runtime from imports + `prompt_building` | Tool/relic/agent cards | Secrets |
| **Transcript** | history / compaction | What happened | — |

**Harness content rule** (`manifests/harness/README.md`):

> Wire laws only. No meta-PE. Every harness line is paid on **every** generation.

### 2.3 Harness tiers

| File | When |
|---|---|
| `manifests/harness/small.md` | children / narrow specialists |
| `manifests/harness/default.md` | parents / workers (default) |
| `manifests/harness/medium.md` | more agent/pipe detail |
| `manifests/harness/big.md` | stubborn models / full reference |

**Expert default:** parent `default` or `medium`; sub-agents `small` unless compliance fails.

Wire in agent.yml (modern):

```yaml
context:
  harness: ../../harness/default.md   # paths relative to agent.yml
  system: ./system-prompts/system.md
  persona: ./system-prompts/persona.md
```

Legacy still seen:

```yaml
persona:
  agent: ./system-prompts/agent.md    # often used as combined system+persona blob
```

Prefer **split** harness/persona/system when the agent is non-trivial. Combined blob is OK for tiny specialists.

### 2.4 Persona engineering (identity PE)

Good persona (`manifests/persona/default.md` pattern):

- **Name + role** in one breath
- **Identity bullets** (craftsman, precise, independent)
- **Values table** (brevity, precision, tool-first…)
- **Operator relationship** (who Mohamed is; how to take correction)
- **Anti-tone** (no cheerleading, no “Great question”)

Bad persona:

- Restates XML protocol
- 2k words of philosophy
- Contradicts harness (e.g. “always explain before tools” vs bare-text ban)
- Generic ChatGPT sludge

### 2.5 System prompt engineering (domain PE)

Put here:

- Mission / out-of-scope
- How to use *this* agent's tool surface
- Quality bar and definition of done
- Delegation policy (when to call sub-agents)
- Domain invariants (build commands, paths, safety)

Do **not** re-teach `<action>` grammar — harness owns that.

### 2.6 `import.files` modules

Static PE attachments. Use for:

- Stable API contracts
- Grading rubrics
- Long checklists you don't want in persona
- Shared fragments across agents

```yaml
import:
  files:
    - ./prompts/review-rubric.md
    - ./prompts/output-contract.md
```

Re-read on agent load. **Not** a sandbox mount.

### 2.7 `prompt_building` (token economy at the card layer)

```yaml
prompt_building:
  runtime_capabilities:
    input_schemas: disable      # default: cheap tool cards
    return_schemas: false
    usage_examples: false
```

| Setting | When true/enable |
|---|---|
| `input_schemas` | Weak models or novel custom tools |
| `return_schemas` | Model must shape JSON returns carefully |
| `usage_examples` | New tools, low-compliance models |

**Expert default for strong models:** all cheap/off.  
**Manifest-expert agent itself** turns schemas on — teaching surface. Don't copy that into every worker.

---

## 3. Protocol contract (what PE must obey)

**Authority:** `docs/protocol/CANON.md` — if harness/docs/tests disagree, **CANON wins**, then fix the other side.

Harness must teach (CANON §10):

1. Closed tag set + who emits what  
2. Exact `<result>` shape  
3. Bare text does not complete  
4. No `final="true"` beside `<action>` in the same generation  
5. Pipe forms that resolve  
6. Id uniqueness for the run  
7. mode / depends_on legality  

Harness must **not**: long ❌ phrase lists for the model to imitate; claim unenforced physics; document undefined behavior without hard reject.

Closed tags (model emits): `<thought>` · `<action>` · `<response>` / `<response final="true">`  
Runtime injects: `<result>` · `<context_feed>`

Synonyms often accepted for thought: `<think>`, `<thinking>` — prefer teaching canonical `<thought>`.

---

## 4. Harness / compliance engineering (meta-PE loop)

When the model misbehaves structurally, you are in **harness-tuner** territory. Do it inside this skill when shipping packages.

### 4.1 Diagnose before edit

| Failure class | Symptom | First move |
|---|---|---|
| Bare-text narration | “I'll check…” before tags | Role + start-of-harness self-check (see tiers) |
| Bare simple answer | `4` not wrapped | Explicit trivial final example |
| Serial vs parallel | One action/turn | Parallel section + multi-action example |
| Missing pipe | Pastes result text | `${id.field}` + depends_on example |
| Wrong tag / attr drift | invents tags, drops `final` | Exact schema + one failed-turn consequence |
| Tail leakage | XML then chatter | “nothing after final” rule + example |
| Premature final | final + action same gen | Loop law restatement |

Baseline: 3–5 runs, count class frequency, **then** edit.

### 4.2 Technique stack (ROI order)

**Tier 1 — always for weak/medium compliance**

1. **Self-check at file start** (position 0 attention) — only if tier > small and model needs it; **conflict:** harness README bans meta-PE slogans. Prefer **structural** openers that stay wire-true:

   ```
   Emit only protocol tags. Untagged text does not complete a turn.
   ```

   Reserve heavy box-drawing self-checks for `big.md` / stubborn models — not every child.

2. **Failed-turn + consequence** using real runtime error shapes (`protocol_error`, bare-text correction) — show what the transcript will contain next.

3. **One correct parallel + pipe example** with real attribute names.

**Tier 2 — targeted**

- Exact-phrase mirroring of the model's favorite sin  
- Trivial final-only path  
- Agent `op=` / inspect attrs when sub-agents misused  

**Tier 3 — runtime when prompt plateaus**

- Stronger model / lower temperature  
- `runtime.mode: autonomous` completion policy (careful)  
- Tighter tool surface (fewer choices → fewer protocol sins)  
- Harness tier up (`small` → `default` → `medium` → `big`)  
- Measurement harness / protocol tests — not more adjectives  

### 4.3 Measurement

Per-capability rates, not vibes:

- tag validity  
- bare-text free  
- final discipline  
- pipe use when multi-step  
- parallel when independent  

Target: daily-driver bar leans **≥90% protocol compliance** (AGENDA).  
One technique per iteration. Minimal diffs.

### 4.4 Tension: harness README vs classic PE

| Pressure | Resolution |
|---|---|
| README: wire laws only | Default for `small`/`default` |
| Stubborn model needs meta-PE | Escalate tier to `big.md` or agent-local harness fork — don't pollute global `default.md` without evidence |
| Persona wants chatter | Persona tone ≠ bare text; protocol still owns completion |
| Long persona + long harness | Cut persona first; harness is per-generation tax |

**Never** “fix” compliance by loosening CANON in the persona (“you may answer in prose”).

---

## 5. Manifest graph (YAML surfaces)

### 5.1 Canon map

| Surface | Canonical | Status |
|---|---|---|
| Sandbox | `docs/manifests/sandbox.md` | SHIPPED |
| Compaction | `docs/manifests/compaction.md` | SHIPPED MVP |
| Agent schema sketch | `docs/manifests/agent.schema.yml` | living |
| Quick ref | `docs/manifests/MANIFEST_QUICK_REFERENCE.md` | living |
| Protocol law | `docs/protocol/CANON.md` | authority |
| Harness tiers | `manifests/harness/` | SHIPPED |
| Schema encyclopedia skill | `manifest-schemas` | skill |
| In-harness orchestrator | `config/agents/manifest-expert/` | agent + sub-experts |

### 5.2 Iron rules

1. Exact `kind:`  
2. Paths relative to the YAML that references them  
3. No code at module root — `src/<concern>/`  
4. README on shippable components  
5. kebab-case dirs · snake_case keys  
6. **`import.files` ≠ sandbox mounts**  
7. Absent `compaction:` = off; absent `sandbox:` = no policy  
8. `history_cap` seatbelt never replaced by compaction  
9. Don't invent top-level keys — extend shipped docs or propose  
10. Rebuild binary after loader changes  

### 5.3 Agent skeleton (production-minded)

```yaml
kind: Agent
name: my-agent
version: "1.0"
summary: "One sentence mission."

cognitive_engine:
  primary:
    provider: deepseek          # must exist on host
    model: deepseek-chat
    parameters: { temperature: 0.3, max_tokens: 8192 }
  fallback:
    provider: openrouter
    model: meta-llama/llama-3.1-8b-instruct

# Prefer context: split when PE is non-trivial
context:
  harness: ../../../manifests/harness/default.md
  system: ./system-prompts/system.md
  persona: ./system-prompts/persona.md
# persona.agent: still OK for combined blob specialists

runtime:
  max_iterations: 24
  history_cap: 80
  max_turns_per_cycle: 15
  action_timeout_sec: 30
  # mode: normal | autonomous

prompt_building:
  runtime_capabilities:
    return_schemas: false
    usage_examples: false

# sandbox:     # see §5.5 — add when boundary needed
# compaction:  # see §5.6 — add for long-horizon

import:
  tools: [exec, grep, list, fs_read, fs_write]
  # feeds: []
  # agents: []
  # files: [./prompts/contract.md]
```

### 5.4 Portable module tree

```
<module>/
├── README.md
├── agent.yml
├── system-prompts/
│   ├── persona.md
│   └── system.md          # or single agent.md if tiny
├── prompts/               # import.files modules
├── tools/ · feeds/ · workflows/ · relics/
├── agents/                # nested full modules
└── context/               # optional seed material for binds
```

Sub-agent `name:` in child `agent.yml` must match parent's `import.agents` reference. Import graph **acyclic**.

### 5.5 Sandbox (SHIPPED)

Canonical detail: `docs/manifests/sandbox.md`.

```yaml
sandbox:
  mode: process                 # process | docker | chroot
  image: alpine:3.19            # docker
  network: out                  # none | out | full
  readonly: false
  allowed_commands: [ls, cat, python3]
  allowed_paths: ["./"]
  allowed_hosts: []             # [] = web_fetch blocked
  files: [./seed.txt]           # LIVE → /workspace/<basename>
  bind:
    - ./ctx:/workspace/ctx
    - ./fixtures:/workspace/fixtures:ro
```

| Intent | Pattern |
|---|---|
| Prompt-only policy text | `import.files` |
| Live editable tree | `sandbox.bind` |
| Untrusted exec | `mode: docker` + RO fixtures |
| No network tools | `allowed_hosts: []` |

### 5.6 Compaction (SHIPPED MVP)

Canonical: `docs/manifests/compaction.md`.

```yaml
runtime:
  history_cap: 80
  max_turns_per_cycle: 15

compaction:
  enabled: true
  profile: balanced             # light | balanced | aggressive | archive_first
  trigger: { context_tokens: 60000, turns: 15 }
  cooldown: { min_turns: 2 }
  overrides:
    tags: { thought: { keep: none }, result: { keep_last: 20 } }
    never_drop: [pin, open_ask]
  output:
    mode: summarize_rules
    archive: { enabled: true, sink: file, format: markdown }
```

| Knob | Affects model prompt? |
|---|---|
| `history_cap` (+ every_turns) | yes — dumb seatbelt |
| `compaction:` | yes — smart; absent=off |
| UI `/truncate` | **no** — TUI only |

### 5.7 Other kinds (compress)

**Tool** — `kind: Tool`; runtime+entrypoint; JSON argv in → JSON stdout; schemas+examples; `src/` layout.  
**Feed** — poll interval; no params; fast idempotent JSON.  
**Relic** — managed/remote; health; endpoints; not a substitute for core session.  
**Workflow** — steps graph; readable > clever.

Full schemas: load `manifest-schemas` or kind expert prompts under  
`config/agents/manifest-expert/agents/*-expert/system-prompts/`.

### 5.8 Specialist lenses (from in-tree orchestrator)

When the task spans kinds, think like `config/agents/manifest-expert`:

| Lens | Owns |
|---|---|
| agent-expert | Agent graph, PE paths, cognitive_engine, sub-agents |
| tool-expert | Tool contracts & scripts |
| feed-expert | Pollers & output_schema |
| workflow-expert | Pipelines |
| relic-expert | Services / health / endpoints |

You may implement all lenses yourself in-session; spawn Cortex sub-agents only when the user runs that orchestrator.

---

## 6. Design workflow (package from zero)

```
1. Mission one-liner + operator
2. Capability surface
   - tools (builtin vs custom)
   - sub-agents? feeds? relics?
3. Trust boundary → sandbox sketch
4. Horizon → history_cap / compaction
5. PE stack
   - harness tier
   - persona (identity)
   - system (mission)
   - modules (stable contracts)
   - prompt_building cost
6. cognitive_engine fit (model strength ↔ PE/harness tier)
7. Write tree + YAML
8. Verify load
9. Smoke protocol behavior if PE/harness changed
10. Tighten imports and PE weight
```

### Model ↔ PE coupling

| Model strength | Harness | prompt_building | Notes |
|---|---|---|---|
| Strong (Grok/GPT-class) | default/small | schemas off | Thin PE, sharp persona |
| Medium | default/medium | examples on for custom tools | Measure compliance |
| Weak / free tier | medium/big | schemas+examples on | Narrow tools; short loops |
| Sub-agent worker | small | off | Inherit protocol from parent transcript norms |

Temperature: specialists 0.1–0.3; creative 0.6–0.8; never “fix” protocol with temperature alone.

---

## 7. Verification

### Load / graph

```bash
make cortex-mk3                                 # if loader/runtime changed
./cortex-mk3 --dry-run -m path/to/agent.yml
# expect: provider/model ok, harness/persona paths ok, sandbox on/off truthful
```

### Surface tests

```bash
make test-policy
make test-sandbox-context
make test-manifest-semantics   # may include known unrelated fails — don't “fix” drive-by
```

### Protocol / PE (when harness or persona policy changed)

- Multi-run smoke: trivial final · single tool · parallel+pipe · bare-text temptation  
- Compare failure classes (§4.1)  
- Optional: protocol test targets under `tests/protocols/`, `src/testing/protocol_test.cpp`  
- Deep compliance campaigns → `harness-tuner` measurement protocol  

### Review scorecard

| Layer | PASS means |
|---|---|
| Graph | kinds valid, imports resolve, acyclic, paths exist |
| PE split | harness=wire, persona=identity, system=mission |
| Harness tier | matches model + role; no domain sludge in harness |
| Sandbox | gates match trust; files vs import.files correct |
| Compact | intentional; seatbelt present |
| Prompt cost | schemas/examples justified |
| Protocol fit | CANON-aligned; no persona vs harness contradiction |

---

## 8. Anti-patterns

| Don't | Do |
|---|---|
| Chatbot persona + hope | Protocol harness + identity persona |
| Protocol tutorial inside persona | Harness tier / CANON projection |
| Meta-PE slogans in global `default.md` without data | Agent-local harness fork or `big.md` |
| Mount `import.files` | `sandbox.bind` for live FS |
| 20 tools on a specialist | 3 tools + delegate |
| Full schemas on strong models “for quality” | Cheap cards; enable when failing |
| Compaction without `history_cap` | Both |
| Treat `/truncate` as context fix | Compaction / history_cap |
| Loosen CANON to fix bare text | Tighten PE + measure |
| Duplicate harness laws in system.md | Single source (harness) |
| Absolute paths in YAML | Relative to manifest dir |
| Dry-run on stale binary | Rebuild |

---

## 9. Quick examples

**A. New review specialist**

1. Module under `config/agents/reviewer/`  
2. `harness: small` · persona: terse critic · system: rubric + out-of-scope  
3. `import.files: [./prompts/rubric.md]`  
4. tools: `fs_read, grep, list` — no exec unless needed  
5. `sandbox.readonly: true`  
6. no compaction  
7. dry-run + one bare-text temptation smoke  

**B. Morpheus-class daily driver**

1. `harness: default|medium`  
2. Rich persona + system mission  
3. Broad but deliberate tools + sub-agents  
4. `compaction.profile: balanced` + high `history_cap` + `max_turns_per_cycle: 15`  
5. sandbox process with real cmds  
6. schemas off  

**C. Model bare-texts on trivial answers**

1. Classify failure (§4.1)  
2. Do **not** edit sandbox  
3. Escalate harness tier or add trivial-final example in **agent-local** harness  
4. Measure 5 runs  
5. If still failing → stronger model / fewer tools (Tier 3)  

**D. “Context keeps dying”**

1. Inspect `history_cap`, `max_turns_per_cycle`, `compaction`  
2. UI truncate is irrelevant  
3. Pins / open_ask in `never_drop`  
4. Patch YAML from compaction.md  

---

## 10. Relationship map

```
manifest-architect                    ideation · 20-flood · filter · skeleton
    └─ handoff ship → manifest-expert (this skill)
         ├─ uses canon docs + harness README + CANON
         ├─ delegates depth → manifest-schemas (schemas)
         ├─ delegates pure compliance grind → harness-tuner
         └─ mirrors config/agents/manifest-expert (+ kind sub-experts)
```

In-harness orchestrator path:  
`config/agents/manifest-expert/agent.yml` + `system-prompts/manifest-expert.md`

---

## 11. Validation / sync

1. Edit **`manifests/skills/manifest-expert/SKILL.md`** first.  
2. After validation, sync:

```bash
cp -a manifests/skills/manifest-expert/SKILL.md .pi/skills/manifest-expert/SKILL.md
cp -a manifests/skills/manifest-expert/SKILL.md ~/.pi/agent/skills/manifest-expert/SKILL.md
```

3. `/reload` or new pi session.  
4. Global register: `~/.pi/agent/settings.json` → `skills` includes  
   `~/.pi/agent/skills/manifest-expert`.

---

*Full stack or it doesn't ship. YAML without PE is cosplay; PE without graph is a prompt in a bottle.*
