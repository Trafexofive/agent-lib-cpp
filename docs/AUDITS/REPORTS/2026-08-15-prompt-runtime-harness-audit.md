# Prompt Building & Runtime Harness Audit — Cortex MK3 vs the Field

**Date:** 2026-08-15
**Status:** Evidence-backed, actionable
**Question answered:** "We don't want cortex_mk3 to be lesser in prompt building and runtime" — is it? Against **pi** (the incumbent coding harness), **Hermes** (Nous), **Manus**, **smolagents**, **Claude Code**, and the broader pioneering field.

**Sources:** primary docs/source only — no blog-speculation:

- pi `0.84.1` — `dist/core/system-prompt.js`, `dist/core/messages.js`, `dist/core/compaction/compaction.js`, and `@earendil-works/pi-agent-core/dist/agent-loop.js` (read from node_modules).
- Hermes — `github.com/NousResearch/hermes-agent` README + `Hermes-Function-Calling`.
- smolagents — `github.com/huggingface/smolagents` README + default CodeAgent system prompt (verbatim).
- Claude Code — Anthropic "Best practices" + "How Claude Code works".
- Manus — "Context Engineering for AI Agents: Lessons from Building Manus" (Peak Ji, Jul 2025).

> Note: the `web_search` SearXNG backend was degraded (empty/PubMed noise) at audit time, so everything was assembled via direct `web_fetch_page` against primary sources. Confidence is marked per system.

---

## 0. Verdict up front

MK3 is **not** lesser. On the dimensions that define a *harness* — protocol ownership, prompt layering, subagent isolation, completion discipline, state machine — it is **peer-or-better** than pi, Hermes, smolagents, Claude Code, and Manus.

Where it is genuinely behind is a *different* discipline: **context engineering** — KV-cache awareness, restorable compression, and attention recitation — which Manus proved at production scale is where agents actually win or lose. That gap is small, concrete, and non-architectural. It is the thing to close, not prompt-layer parity.

---

## 1. pi (`@earendil-works/pi-coding-agent` 0.84.1)

**Confidence: HIGH** (read from local node_modules).

### Prompt building — pi is a flat string

`buildSystemPrompt()` (`dist/core/system-prompt.js`) emits ONE concatenated blob:

1. Fixed boilerplate ("You are an expert coding assistant operating inside pi…").
2. `Available tools:` flattened as `- name: one-line-snippet`. Only tools *with* snippets appear — silent drop otherwise.
3. `Guidelines:` hardcoded + caller `promptGuidelines`, Set-deduplicated.
4. Pi documentation pointer block (static list of "when asked about X, read Y").
5. `appendSystemPrompt` — where AGENTS.md / USER.md / skills glue in.
6. `<project_context>` wrapping `contextFiles` as `<project_instructions path="…">`.
7. Skills section (only if `read` tool present).
8. `Current working directory: <cwd>`.

**Only genuine strength:** one-line tool cards = cheap tokens (≈ MK3 `prompt_building` with schemas off). Everything else is a catch-all appendix.

| MK3 concept | pi | Verdict |
|---|---|---|
| Layered `system` XML (harness/persona/system/modules) | one flat string | **MK3 wins** — pi can't token-cache static vs dynamic regions |
| Harness tiers (small/default/medium/big) | none, one boilerplate | **MK3 wins** |
| Layer responsibility contract | none enforced | **MK3 wins** |
| Persona as first-class layer | no persona concept | **MK3 wins** |
| Module injection (`import.files`) | only `contextFiles` + skills | **MK3 wins** |
| Protocol teaching | none — pi uses native tool-calls | structural fork (§3) |

### Runtime loop

The loop is **not pi** — it lives in `@earendil-works/pi-agent-core`. `runLoop()`: outer loop re-arms on queued/steer messages; inner loop streams assistant → native `toolCall` blocks → execute (parallel default) → append `toolResult` → repeat until no calls. Config hooks: `transformContext`, `convertToLlm`, `beforeToolCall`/`afterToolCall`, `shouldStopAfterTurn`, `prepareNextTurn`, `getSteeringMessages`, `toolExecution`.

| pi | MK3 | Verdict |
|---|---|---|
| `getSteeringMessages` (mid-turn steer) | user interrupt / context steer | parity |
| native tool-calls | XML `<action>` + state machine | structural divergence |
| `beforeToolCall`/`afterToolCall` | sandbox gate + policy | parity |
| binary parallel/sequential | `sync|async|fire_and_forget` + `depends_on` DAG | **MK3 wins** |
| truncated-call defense (`stopReason==="length"` → fail all) | `timeout` + `protocol_error` status | parity |

### Compaction — the one place pi is ahead

pi ships **branch summarization** (fork/rewind → `<summary>` re-injection) and **split-turn / previous-summary chaining** — neither of which MK3's MVP documented. (Both spec'd into `docs/manifests/compaction.md` during this audit as PROPOSED.)

### Protocol model — the real fork

| | pi | MK3 |
|---|---|---|
| Tool interface | native tool-calls (provider) | XML tag protocol (sovereign) |
| Completion | `stopReason` | `<response final="true">` in-protocol |
| Bare text | falls through as text | STRICT fail + runtime correction |
| Compliance | N/A (provider enforces) | first-class, measured |

**Reading:** pi bought simplicity by delegating tool grammar to the provider. MK3 owns the wire contract (CANON) deliberately — the stochastic-containment thesis. Not a gap to close; it's the moat.

---

## 2. Hermes Agent (Nous Research)

**Confidence: HIGH** (primary README + Function-Calling repo).

**What it is:** Python agent; the differentiator is a **closed learning loop**, not a novel protocol. `SOUL.md` persona, `MEMORY.md`/`USER.md` persistence, `AGENTS.md` workspace instructions.

- **Protocol** (`Hermes-Function-Calling`): XML `<tool_call>`/`<tool_response>`, plus `<scratch_pad>` with a **GOAP** sub-structure (`Goal:`/`Actions:`/`Observation:`/`Reflection:`) — structured reasoning *inside* the tool-call grammar. Direct cousin of CANON.
- **Runtime:** ReAct loop; subagent spawning; **RPC script-collapse** (Python scripts calling tools over RPC → "multi-step pipelines into zero-context-cost turns").
- **Memory moat** (the real takeaway): agent-curated memory with **nudges**; **autonomous skill creation** that **self-improves during use**; **FTS5 session search + LLM summarization**; **Honcho dialectic user modeling**; `agentskills.io` standard.
- **Trajectory compression** to train the next generation of tool-calling models (eats its own traces).

**Verdict:** protocol is a CANON cousin (MK3's is stricter/more principled). The horizontal memory/skill loop is out of MK3's MVP scope, not a harness regression.

---

## 3. Manus — "Prime" is a phantom; the context engineering is the gold

**Confidence on "Prime": HIGH that it doesn't exist.** No canonical Manus "Prime" in docs/blog/repos (`github.com/manusash/Prime` → 404). Product line is 1.x/Plan Mode/Branch/Wide Research/etc.

**What matters is Manus's context engineering** (Peak Ji's post — the single most valuable primary source in this audit):

1. **Design around the KV-cache** — #1 metric is *KV-cache hit rate*. Append-only context · deterministic serialization · **no timestamp in the system prompt** (one-token diff invalidates the prefix) · explicit cache breakpoints.
2. **Mask, don't remove** — tool definitions sit near the context front; mid-iteration tool add/remove nukes the cache *and* confuses dangling action refs. Manus uses a **context-aware FSM + logit masking** (via response *prefill*) to constrain the action space without touching definitions. Consistent action-name prefixes (`browser_`, `shell_`) select tool *groups* per state.
3. **File system as context** — unlimited/persistent/operable. Compression is always **restorable**: drop web-page *content*, keep the *URL*; drop document text, keep the *path*.
4. **Attention recitation** — the `todo.md` Manus constantly rewrites is deliberate: it recites objectives into the *end* of context to defeat lost-in-the-middle / goal drift (~50 calls/task).
5. **Keep the wrong stuff in** — don't erase failures; error recovery is the clearest agency signal; a failed action+stacktrace is evidence that shifts the prior.
6. **Don't get few-shotted** — inject small serialization *diversity* so the model doesn't mimic its own past rhythm into a rut.

**Verdict:** Manus and MK3 are building the *same* thing (sovereign, model-agnostic, append-only transcript + state machine). Manus has the production-scale data proving these context rules. **This is MK3's real gap.**

---

## 4. smolagents (Hugging Face) — code-as-action

**Confidence: HIGH** (default system prompt verbatim).

- **CodeAgent:** action is a Python snippet, not a dict/XML ("agents that think in code"). Claimed 30% fewer steps.
- **Prompt** is a **Jinja template** with `{{placeholders}}` (`tools`, `managed_agents`, `authorized_imports`, `custom_instructions`); `instructions=` **appends**.
- Mandatory **Thought/Code/Observation** cycle ("else you will fail").
- **Completion = calling `final_answer()`** — a *tool* acts as the terminal signal, not a flag. Cleanest completion idiom in the field; direct analog to MK3's `<response final="true">`.
- `planning_interval` — optional non-tool planning step every N steps.

**Verdict:** proves "code blobs > dict blobs" and the completion-signal-as-tool idiom. MK3 shares the principle (structured action + mandatory completion) via XML instead of code.

---

## 5. Claude Code (Anthropic) — the production reference

**Confidence: HIGH** (official docs).

- **Layering** (closest to MK3's split): `CLAUDE.md` (project conventions, loaded every session) · `SKILL.md` (on-demand, `disable-model-invocation` for side-effects) · **subagents** (`.claude/agents/*.md`, own context + tool allowlist + model) · **hooks** (deterministic) vs CLAUDE.md (advisory). The advisory-vs-deterministic split *is* MK3's harness/runtime contract.
- **Context:** auto-compaction (summarizes code patterns/file states/decisions), `/compact <instructions>`, `/rewind` checkpoint + file snapshots, `/clear`.
- **Auto-mode:** a *separate classifier model* gates command approval.
- **The steal:** the **verification subagent** — a fresh-context reviewer that sees only the diff + criteria, not the reasoning that produced it (grader ≠ author). MK3's BUDDY/FORGE is adjacent but not wired as a finalization gate.

---

## 6. Cross-field convergence table

| Technique | Who ships it | MK3 status |
|---|---|---|
| XML/structured tag protocol + runtime-injected results | Hermes, MK3 (CANON) | ✅ SHIPPED |
| In-protocol completion signal | smolagents (`final_answer`), MK3 (`final="true"`) | ✅ SHIPPED |
| Layered prompt (persona/system/modules/harness) | Claude Code, MK3 | ✅ SHIPPED |
| Subagents with isolated context + own tools | Claude Code, Hermes, MK3 | ✅ SHIPPED |
| State machine for action gating | Manus, MK3 | ✅ SHIPPED |
| **Append-only / KV-cache-aware context** | Manus | ⚠️ **GAP** |
| **Logit masking (not tool removal)** | Manus | ⚠️ **GAP** |
| **Restorable compression** (drop content, keep pointer) | Manus | ⚠️ **GAP** (`truncate_chars` is lossy) |
| **Attention recitation** (`todo.md` → context end) | Manus | ⚠️ **GAP** |
| **Keep failures in context** | Manus, Claude Code | 🔶 partial (`on_error: keep`, no explicit law) |
| Anti-fewshot diversity | Manus | ❌ absent |
| Self-improving memory/skills | Hermes | ❌ out of scope |
| Adversarial verification (fresh context) | Claude Code | 🔶 adjacent, not gated |
| RPC script-collapse | Hermes | ❌ absent |

**MK3 is at/above frontier on the harness spine; behind on context engineering — all of it higher ROI than more prompt layers.**

---

## 7. Recommendations (ranked ROI ÷ effort)

### Tier 1 — cheap, high-value, steal this cycle

1. **Append-only + deterministic serialization + stable prefix** as a **CANON law** (not advice). No timestamps in the canonical system prefix; canonical key ordering. MK3's single biggest latent cost/latency win; ~free to spec.
2. **Restorable compaction** — extend `compaction.md`: instead of `truncate_chars` *dropping* content, emit `[pointer: <url|path|id>]` so the model can re-fetch. "Drop content, keep pointer" is strictly better.
3. **Attention recitation primitive** — a first-class mechanism (e.g. `recite()` appending the active plan to context end every N turns). MK3 already has plan/goal state; reciting it into *recent* context is the missing half.

### Tier 2 — spec now, ship later

4. **Logit-mask action gating** (Manus prefill) — only if the tool surface explodes into the hundreds. Document as the "mask, don't remove" law to prevent naive dynamic tool-unloading.
5. **Adversarial verification subagent gate** (Claude Code) — wire BUDDY/FORGE fresh-context review as an *optional* `runtime` finalization-gate.

### Tier 3 — out of MVP, note only

6. Hermes memory/skill self-improvement loop · anti-fewshot serialization diversity · RPC script-collapse.

---

## 8. Bottom line

**"Don't want MK3 to be lesser in prompt building and runtime" → it isn't.** On protocol ownership, prompt layering, subagent isolation, completion discipline, and state machines, MK3 is peer-or-better than pi, Hermes, smolagents, Claude Code, and Manus.

**The gap is context engineering** — KV-cache awareness, restorable compression, attention recitation — where Manus (production-scale proof) and Claude Code (verification) and Hermes (memory) each contribute a piece MK3 lacks. Small, concrete, non-architectural. Close that, and MK3 moves from "peer harness" to "best-in-class on the dimension that actually separates winning agents."

---

## Appendix — artifacts

- `pi-prompt-runtime-audit` (`art-msuf9vwa-ijvfgb`) — full pi vs MK3 deep-dive.
- `frontier-harness-audit` (`art-msuiuzd1-g2bvmj`) — full Hermes/Manus/smolagents/ClaudeCode synthesis.
- `docs/manifests/compaction.md` — updated during this audit with PROPOSED branch-summary + split-turn chaining + (pending) restorable-pointer sections.
