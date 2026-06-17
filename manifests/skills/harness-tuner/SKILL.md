---
name: harness-tuner
description: >
  Fine-tune agent harness system prompts to enforce structural LLM compliance:
  XML-only output, no bare-text narration, parallel execution patterns, data
  piping, response tagging. Use this skill whenever the task involves iterating
  on a system prompt / harness file to fix LLM behavior — wrong output format,
  missing tags, narration leaking outside tags, serial-when-should-be-parallel,
  or any structural compliance failure. Also use for: writing a harness from
  scratch, diagnosing why an LLM ignores a protocol, measuring compliance rate
  across runs, or designing test cases for prompt regressions.
---

# Harness Tuner

Narrow skill for prompt-engineering an agent harness to achieve structural
compliance from an LLM. "Structural compliance" means: the LLM outputs exactly
the format the runtime parser expects — correct tags, no bare text, correct
attributes, correct sequencing.

---

## 1. Diagnose First

Before touching the harness, classify the failure. Every fix maps to one of
these:

| Failure class | Symptom | Root cause |
|---|---|---|
| **Bare-text narration** | "I'll check X..." before tags | Conversational default overriding protocol |
| **Bare simple answer** | `4` instead of `<response final="true">4</response>` | Protocol not covering trivial cases explicitly |
| **Serial when parallel** | One action per turn | No parallelism instruction or weak framing |
| **Missing piping** | Copies output into next action body | No `${id.field}` instruction |
| **Wrong tag** | Uses `<think>` not `<thought>` | Schema ambiguity or missing exact name |
| **Attribute drift** | Omits `final="true"`, wrong `mode` | Rules stated but not exampled |
| **Tail leakage** | Correct XML then bare text at end | Post-tag narration not addressed |

Run 3–5 samples of the failing prompt before changing anything. Record which
failure class appears and at what frequency. That's your baseline.

---

## 2. The Technique Stack

Apply techniques in order of ROI. Stop when compliance rate is acceptable or
you hit diminishing returns.

### Tier 1 — Always apply

**Self-check trigger (first in file)**
Put this as the absolute first content, before any explanation:

```
╔══════════════════════════════════════════════════════════╗
║  BEFORE YOU EMIT: Does your output start with <?        ║
║  If NOT — delete everything before the first <.         ║
║  That text is invisible. Parser strips it. Turn wasted. ║
╚══════════════════════════════════════════════════════════╝
```

The position matters. LLMs weight prompt-start content heavily. A rule buried
in section 4 loses to a conversational prior. A rule at position 0 fires first.

**Failed-turn example with consequence**
Don't just show the wrong thing — show the error it produces:

```
FAILED TURN:
  I'll check git status.          ← STRIPPED
  <action ...>...</action>
  → SYSTEM: <result status="error">BARE_TEXT — turn wasted</result>
  → YOU LOST A TURN.

CORRECT TURN:
  <response>I'll check git status.</response>
  <action ...>...</action>
  → SYSTEM: <result status="ok">...</result>
```

This is more effective than a rule because it shows the LLM what it will
experience, not just what it should do.

**Role reframing**
One sentence, early: `You are a protocol agent — not a chatbot. You speak XML.
Your output is parsed by a state machine.`

This breaks the default "helpful assistant" mode that generates narration.

### Tier 2 — Apply for specific failures

**Exact phrase mirroring**
If the LLM keeps emitting a specific phrase, put that exact phrase in the
wrong/right pair. `"I'll check git status and list files"` in the example is
worth more than a generic `"bare text example"`. The LLM pattern-matches its
own output patterns.

**Coverage of trivial cases**
Bare simple answers are a distinct failure from narration. Cover them
explicitly with their own example:
```
"4"   →  <response final="true">4</response>
"yes" →  <response final="true">yes</response>
```
Don't assume the general rule covers them. It doesn't reliably.

**Compaction**
Twelve weak restatements < one strong block. If the harness has the same rule
in 4 places, consolidate to 1. Token budget spent on repetition dilutes signal.
Target: one canonical statement per rule, best example per concept.

### Tier 3 — Runtime-level (when prompt tuning plateaus)

If you've applied Tier 1 + 2 and compliance is still inconsistent (~33%), the
remaining gap is model non-determinism against a deeply-trained conversational
prior. Prompt tuning has a ceiling here. Options:

- **Strict XML mode**: Runtime rejects turns with bare text, injects a
  `<result status="error">BARE_TEXT</result>` so the LLM sees its own violation
  in context and self-corrects next turn.
- **Warning render**: Display bare text in a visible warning block rather than
  silently dropping it. The LLM learns from seeing the consequence in context.
- **Model switch**: Some models (GPT-4o, Claude) have less conversational bleed
  than deepseek-chat for protocol tasks. Worth testing if runtime fix is blocked.

---

## 3. Measurement Protocol

**Per-capability compliance rate, not overall pass/fail.**

Run each capability 5× minimum (model is non-deterministic). Record per run:
- Did the turn start with `<`? (bare-text gate)
- Was the specific capability exercised correctly? (parallel / piping / tags)

```
Capability          | N | Pass | Rate
--------------------|---|------|-----
Simple answer       | 5 | 5    | 100%
Parallel 2-way      | 5 | 3    | 60%
Parallel 3-way      | 5 | 4    | 80%
Data piping         | 5 | 5    | 100%
Thought tags        | 5 | 5    | 100%
```

Don't average across capabilities — they have different root causes and
different fix paths.

**Baseline before first edit.** You can't measure improvement without it.

---

## 4. Iteration Loop

```
read harness
  │
  ▼
run 3-5 samples per failing capability  ← establish baseline
  │
  ▼
classify failures (§1)
  │
  ▼
pick lowest-effort technique from §2
  │
  ▼
edit harness (minimal diff — one technique per iteration)
  │
  ▼
run same samples again (same prompts, same model)
  │
  ▼
did rate improve?
  ├── yes → record delta, pick next failing capability, repeat
  └── no  → revert, pick different technique, or escalate to §2 Tier 3
```

**One technique per iteration.** If you apply three things at once and it
improves, you don't know which one worked. If it regresses, you don't know
which one broke it.

---

## 5. Harness Structure Checklist

A well-formed harness has these sections in this order:

1. **Self-check block** — first, visually distinct (box border), ≤8 lines
2. **Role statement** — one sentence, sets the frame
3. **Protocol reference** — the four (or N) tags, attributes, body format
4. **Parallel execution** — slow vs fast example, `depends_on` pattern
5. **Data piping** — `${id.field}` syntax, rules, chain example
6. **Turn budget** — max turns before mandatory response
7. **Error recovery** — one line per error class, "never repeat identical failure"
8. **Anti-patterns** — ❌ list, concrete
9. **Examples** — 4–6, numbered, covering: direct answer, parallel gather,
   piped analysis, error recovery, sub-agent, full pipeline

Sections 4, 5, 9 are where most compliance failures get fixed. Don't cut them
for brevity.

---

## 6. Output Format

When reporting a tuning session, use this structure:

```
Baseline (N=5 per capability):
  [table]

Techniques applied (this iteration):
  1. [technique] — [rationale]
  2. ...

Post-edit (N=5 per capability):
  [table]

Delta:
  [capability]: X% → Y% (+Z)

Remaining gap + recommended next step:
  [text]
```

---

## Notes

- `deepseek-chat` has stronger conversational bleed than instruction-tuned
  models. Tier 1 techniques move the needle but don't eliminate it. Plan for
  runtime enforcement.
- The self-check trigger loses effect if the harness grows significantly after
  it — the model's attention window dilutes. Keep the harness tight or repeat
  the self-check at the end.
- `final="true"` omission is a separate bug from bare-text narration. Test it
  explicitly — a model that wraps narration in `<response>` may still forget
  `final="true"` on terminal turns.
- For multi-provider runtimes: tune against the weakest model, not the best.
  Compliance improvements on a strong model may not transfer.
