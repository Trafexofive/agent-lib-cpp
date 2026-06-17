---
name: session-to-skill
description: >
  Convert a pi session into a reusable skill. Use when the user says "turn this session into a skill",
  "create a skill from this conversation", "distill this session", "extract a skill", "save this
  workflow as a skill", "make a skill from what we just did", or any variation of converting session
  history into a reusable SKILL.md. Also trigger when the user references a past session and wants it
  packaged for reuse.
---

# Session-to-Skill Converter

Distill a pi session into a reusable, production-quality SKILL.md.

## When This Skill Activates

The agent loads this skill when the user wants to convert a session (current or past) into a skill. Do not wait for the user to type `/skill:session-to-skill` — the description above triggers on natural language.

## Tool Usage Rules (Pi Agent Global Config)

This skill runs inside pi. You have access to the full agent toolset. Use these rules:

### Session Discovery

| Task | Tool | Why |
|------|------|-----|
| List session directories | `bash` — `ls -lt ~/.pi/agent/sessions/*/` | One command, sorted by recency |
| Find session by keyword | `bash` with a grep loop | `find` + `grep` across JSONL files |
| Extract session into structured text | `bash` with the Python scripts below | Strip metadata scaffolding; keep only user/assistant/toolResult content |
| Read extracted content | pi's `read` tool on the extract file | Extracted text is 60% denser than raw JSONL — more signal per token |

**Never `read` a raw `.jsonl` file directly.** A 40-exchange session is ~60% metadata (session entries, model changes, thinking toggles). Always run the extraction scripts first → write to a temp file → `read` the temp file. The only exception: if the session is the CURRENT session (already in context), skip file I/O entirely and scan context directly.

### Content Extraction Pattern

```bash
# Extract ALL user messages (no truncation — let the agent read the whole thing)
python3 -c "
import sys, json
for line in sys.stdin:
    d = json.loads(line)
    if d.get('type') != 'message': continue
    msg = d.get('message', {})
    if msg.get('role') != 'user': continue
    content = msg.get('content', '')
    if isinstance(content, list):
        for part in content:
            if isinstance(part, dict) and part.get('type') == 'text':
                print(part['text'])
    elif isinstance(content, str):
        print(content)
    print('\n--- USER MESSAGE ---\n')
" < session.jsonl
```

```bash
# Extract ALL assistant messages (tool calls and text responses)
python3 -c "
import sys, json
for line in sys.stdin:
    d = json.loads(line)
    if d.get('type') != 'message': continue
    msg = d.get('message', {})
    if msg.get('role') != 'assistant': continue
    content = msg.get('content', '')
    if isinstance(content, list):
        for part in content:
            if isinstance(part, dict):
                if part.get('type') == 'text':
                    print(part['text'])
                elif part.get('type') == 'tool_call':
                    print(f'[TOOL: {part.get(\"name\",\"?\")}] {json.dumps(part.get(\"arguments\",{}))[:200]}')
    elif isinstance(content, str):
        print(content)
    print('\n--- ASSISTANT ---\n')
" < session.jsonl
```

```bash
# Extract tool results
python3 -c "
import sys, json
for line in sys.stdin:
    d = json.loads(line)
    if d.get('type') != 'message': continue
    msg = d.get('message', {})
    if msg.get('role') != 'toolResult': continue
    tool_name = msg.get('toolName', '?')
    content = msg.get('content', '')
    text = ''
    if isinstance(content, list):
        for part in content:
            if isinstance(part, dict) and part.get('type') == 'text':
                text += part['text']
    elif isinstance(content, str):
        text = content
    print(f'[TOOL RESULT: {tool_name}] {text[:2000]}')
    print('---')
" < session.jsonl
```

### Important: No Silent Truncation

- **Never truncate session extraction with `[:500]` or similar.** The agent needs full context to identify patterns. If output is large, pipe to a temp file and `read` it in chunks.
- If a session file is huge (>500KB), read it in chunks with `read --offset N --limit M` rather than truncating.
- **Surface parse errors.** If a JSONL line fails to parse, report it — don't suppress with `2>/dev/null`. A corrupt line might be the one containing the key insight.

### Handling Large Output

```bash
# For large extractions, write to a temp file first
python3 -c "..." < session.jsonl > /tmp/session-extract.txt
```

Then read it into context. Use pi's `read` tool with offset/limit (preferred — chunked reads without spawning):
```
read /tmp/session-extract.txt --offset 1 --limit 200
```

If the `read` tool is unavailable, fall back to bash:
```bash
sed -n '1,200p' /tmp/session-extract.txt
```

For structured results, persist as an artifact:
```
artifact_create("session-extract", content, "note")
```

## Phase 1: Identify the Session

The session can come from three sources:

| Source | How to identify |
|--------|----------------|
| **Current session** | The user is right now. No file lookup needed — the agent is IN the session. Extract from the conversation history in context. |
| **Named past session** | User says "session X" or "the session from yesterday about Y". Look in `~/.pi/agent/sessions/` — session directories are named `--<cwd-path>` (with `/` replaced by `-`). |
| **Very short session** | User says "turn this into a skill" but the session is under 5 exchanges — one question, one answer. Skip Phase 2 wholesale. Write a minimal skill directly: Quick Reference (the working command) + one Rule if any constraint was mentioned. A 3-line SKILL.md is fine. |
| **Last session by topic** | User says "the last time we worked on X". Grep recent session files for the topic keyword. |

### Session File Structure

```
~/.pi/agent/sessions/
└── --home-mlamkadm--repos-substrate/   # One dir per cwd
    ├── 2026-04-09T21-36-36-621Z_uuid.jsonl   # One file per conversation
    ├── 2026-04-10T03-03-03-998Z_uuid.jsonl
    └── ...
```

Each `.jsonl` file is newline-delimited JSON. Entry types:

| `type` | Contains |
|--------|----------|
| `session` | Session metadata (id, cwd, timestamp) |
| `message` + `role: "user"` | User prompts |
| `message` + `role: "assistant"` | Agent responses (may include tool calls) |
| `message` + `role: "toolResult"` | Tool execution results |
| `model_change` | Model switch events |
| `thinking_level_change` | Thinking level toggle |

### Finding a Session by Keyword (with Error Surfacing)

```bash
# Search with error visibility
for dir in ~/.pi/agent/sessions/*/; do
  for f in "$dir"/*.jsonl; do
    if grep -l "keyword" "$f" 2>/dev/null; then
      echo "MATCH: $f"
      # Validate file is readable JSONL (pass path as sys.argv, not inline '$f')
      python3 -c '
import sys, json
bad = 0
total = 0
with open(sys.argv[1]) as fh:
    for i, line in enumerate(fh, 1):
        total = i
        try: json.loads(line)
        except: bad += 1
if bad:
    print(f"  WARNING: {bad} unparseable lines in {total} total")
' "$f"
    fi
  done
done
```

### For the Current Session

The conversation history is already in context. Scan it directly — do NOT re-read session files. Your context window already contains the full conversation. Use your own reasoning to extract patterns from what you've already seen.

**Short-circuit for brief sessions:** If the current session is under 5 exchanges (one question, one tool call, one answer), skip Phase 2 entirely. Extract the command and any constraint directly from context and go straight to Phase 3 — write a minimal skill. The full extraction workflow exists for sessions where patterns must be identified from noise.

### Phase 1.5: Scope the Session

**Before extracting, define the exchange range.** A long session may touch multiple unrelated topics — converting the whole thing produces a kitchen-sink skill that nothing triggers cleanly.

1. **If the user specified a topic** ("the Docker part", "when we were debugging CI"): grep user messages for topic keywords to identify the first and last relevant exchanges. Only extract that range.
2. **If the user didn't specify**: scan user messages for topic clusters. If you find 2+ distinct clusters (e.g., one about SQL migrations, one about Docker setup), flag them: "This session has 3 topic clusters: Docker setup (exchanges 1-12), SQL migrations (13-28), CI debugging (29-45). Which do you want skillified?" If there's one dominant topic, scope to the whole session.
3. **Edge case — entangled topics**: if two topics are interleaved (user switched back and forth), extract the user messages for each topic into separate temp files, then process each independently.

**Scoping is mandatory** for sessions with >15 exchanges. Skip for short (<5 exchange) sessions.

## Phase 2: Extract Patterns — Multi-Pass With Signals

**Do not try to extract everything in one pass.** For sessions >10 exchanges, use this 3-pass strategy. Each pass writes to a separate artifact so context is freed between passes.

### Pass 1 — User Messages (Topics + Constraints + Commands)

Read only user messages. Categorize into:
- **Explicit constraints** the user stated ("always do X", "never touch Y")
- **Commands the user ran** (or told you to run) — record the exact command and whether it worked first-try or needed iteration
- **Topic boundaries** — which exchanges are about what
- **Explicit "remember this" markers** — priority 1

Write findings to artifact: `artifact_create("session-extract-pass1", ..., "note")`

### Pass 2 — Assistant Messages + Tool Results (Corrections + Errors + Workflows)

Read only assistant text and tool results. Categorize into:
- **Agent self-corrections** ("actually...", "I was wrong...")
- **Error→resolution chains** — match tool result errors to the assistant message that fixed it
- **Multi-step sequences** (3+ exchanges to reach an outcome = workflow)
- **Wrong assumptions the agent made and was corrected on**

Write findings to artifact: `artifact_create("session-extract-pass2", ..., "note")`

### Pass 3 — Cross-Reference

Read both pass artifacts. Cross-reference:
- Did a user constraint match an agent correction? → elevate to Rule (priority 1)
- Did a command from Pass 1 cause an error resolved in Pass 2? → Pitfall
- Which files were touched across both passes? → Project Map
- Which commands became workflows? → match the sequence

Write the final consolidated extraction: `artifact_create("session-extract-final", ..., "note")`

**For sessions ≤10 exchanges**, you can do all three passes in one read — but still separate the thinking (users first, then assistants, then cross-reference) to avoid missing corrections at the bottom of context.

**Apply these signals** during extraction to determine what matters and where it goes:

### Importance Signal Table

| Signal | Maps to | Priority |
|--------|---------|----------|
| User says "remember this", "note this", "this is important" | Rules or Pitfalls | **1 — highest** |
| Agent corrected itself mid-session ("actually...", "I was wrong...") | Rules — the correction IS the rule | **1** |
| User retried the same command with variations before it worked | Pitfalls — capture what finally worked AND what didn't | **1** |
| Multi-step back-and-forth (≥3 exchanges) to reach an outcome | Workflow — the sequence IS the workflow | **2** |
| Error that was explicitly diagnosed and resolved | Pitfalls — error + root cause + fix | **2** |
| User explicitly stated a constraint or convention | Rules | **2** |
| Agent made a wrong assumption that was corrected | Rules — "never assume X" | **2** |
| Single command, worked first try, user moved on | Quick Reference | **3** |
| File/directory mentioned in passing without discussion | Project Map | **4** |
| Setup commands run once at session start | Setup section | **3** |

**Iterated content > first-attempt content.** A command the user ran 4 times with tweaks is more signal than one they ran once successfully. The final working version goes in Quick Reference; the failed attempts go in Pitfalls.

**Resolved errors = pitfall gold.** An error message alone is useless. An error message + the diagnosis + the working fix is the most valuable content in a skill.

**Deduplicate aggressively.** If the same command appears across multiple exchanges, record it once with the most relevant category and a note about WHEN to run it. If it's run in different contexts (e.g., `cargo build --release` during development vs. during CI), note both contexts but don't list the command twice. Collapse repeated content into a single entry with richer context.

### 2a. Domain & Trigger

What was the session ABOUT? This becomes the skill's `name` and `description`. 

**Description formula — use this template:**

```
Use when [working on / debugging / building / deploying] <specific domain>.
Triggers: <comma-separated natural-language phrases that should activate this skill>.
Does NOT apply to: <false-positive situations where this skill should NOT load>.
```

The "Does NOT apply to" line is critical. Without it, the skill loads on adjacent but unrelated tasks. Example:

```
Use when working on the Smelt compiler — building, testing, debugging, or extending it.
Triggers: "smelt", "compiler", "smelt build", "smelt test", "smelt type check".
Does NOT apply to: general C/C++ build issues unrelated to Smelt, Makefile debugging,
runtime C library work outside the compiler pipeline.
```

### 2b. Skill Granularity: Split vs Merge

A single session might touch multiple domains. Use this heuristic:

- **If the trigger descriptions for two topic clusters wouldn't overlap** → separate skills. A session about "setting up Docker + writing SQL migrations" might yield `docker-workflows` and `db-migrations` as two skills.
- **If they'd co-occur naturally** (same project, same workflow triggers both) → merge. A session about "building the parser + testing the parser" stays one skill: `smelt-compiler`.
- **If in doubt, split.** It's easier to merge two narrow skills later than to split an overbroad one.
- **Name each skill for its domain.** `smelt-frontend` and `smelt-backend`, not `smelt-part1` and `smelt-part2`.

### 2c. Commands & Quick Reference

Every command that was run, especially ones that worked after trial and error.

Format as copy-paste-ready code blocks. Include the context of WHEN to run each command (not just the command itself). Add the WHY when it's non-obvious.

```markdown
## Quick Reference

### Build
```bash
make clean && make          # Clean rebuild — dependency tracker is unreliable, always clean
make debug                  # Debug build with -g and assertions enabled
```

### Test
```bash
make test-quick             # Fast smoke test (~15s) — run before every commit
make test                   # Full suite (~2min) — run before pushing
```
```

### 2d. Workflows

Step-by-step processes that emerged. A workflow is a **sequence with preconditions and decision points**, not just a list of commands.

```markdown
## Workflows

### Adding a Language Feature to the Compiler

1. **Start with tests.** Write a `.smt` example file that exercises the feature.
2. Add entry to `tests/pipeline/suites/current.tsv`.
3. **Build first:** `make clean && make` — verify no regressions before touching code.
4. Implement in order:
   - Parser (`src/frontend/parser.cpp`) — add token type, AST node, parse logic
   - Resolver (`src/middle/resolver.cpp`) — type checking, symbol table
   - Emitter (`src/backend/emitter.cpp`) — C code gen
5. **Test after each layer**, not just at the end: `make test-quick` after parser, then after resolver, then after emitter.
6. If emitter produces broken C, debug with `./build/smelt emit file.smt` to inspect generated code.
7. All 19 pipeline tests must pass. No warnings.
8. Commit after every working layer.
```

Look for:
- Sequences the user repeated
- Ordering constraints ("do X before Y because Z")
- Decision points ("if build fails here, check X; if it fails here, check Y")
- Debugging loops (what was tried, what finally worked — capture BOTH)

### 2e. Rules & Constraints

Non-obvious rules, conventions, or constraints discovered during the session.

Rules come from two sources:
- **Explicit:** the user stated them ("always do X", "never touch Y")
- **Derived from corrections:** the agent did it wrong and was corrected → the correction IS the rule

```markdown
## Rules

1. **Clean rebuild on every change.** `make clean && make`, never `make` alone. The dependency tracker misses header changes in the runtime directory.
2. **Test after each layer, not just at the end.** Parser → test, resolver → test, emitter → test. Catching errors at the parser stage saves 10x debugging time.
3. **Never modify `tests/pipeline/suites/` directly.** Entries are generated by the test harness. Manual edits cause CI mismatches.
4. **Config lives in `config.yml`, never in environment variables.** Single source of truth. Env vars are for secrets only.
```

### 2f. Files & Architecture

Key files, directories, and architectural notes. Include WHY each file matters, not just its path.

```markdown
## Project Map

```
smelt/
├── src/
│   ├── frontend/parser.cpp    # Tokenizer + recursive descent parser
│   ├── middle/resolver.cpp    # Type checker, symbol table, scope resolution
│   └── backend/emitter.cpp    # C code generation — most complex file, handles type lowering
├── tests/pipeline/            # Integration test suites (TSV manifests + expected outputs)
├── examples/                  # Example .smt programs (also used as test fixtures)
└── std/                       # Standard library (.smt) — shipped with compiler
```

### Key Files

| File | Purpose | Edit Frequency |
|------|---------|---------------|
| `src/frontend/parser.cpp` | Tokenizer + parser | High — every language feature touches this |
| `src/backend/emitter.cpp` | C code gen, type lowering | High — most bugs live here |
| `tests/pipeline/suites/current.tsv` | Test manifest | Medium — add entry per feature |
| `src/runtime/gc.c` | Garbage collector | Low — stable, test before touching |
```

### 2g. Pitfalls & Gotchas

Things that went wrong, their root causes, and how to avoid them. **This is the highest-value section.** A pitfall without a root cause is just a symptom list — include the WHY.

```markdown
## Pitfalls

| Trap | Root Cause | Fix |
|------|-----------|-----|
| `make` succeeds but binary segfaults on first run | `config.yml` missing a required key — no schema validation at startup | Run `smelt check --config` before first build, or keep a `config.example.yml` |
| Parser change passes tests but emitter produces broken C | The resolver silently accepts malformed AST nodes the emitter can't handle | After any parser change, run `smelt emit` on test files and inspect generated C — don't rely on pipeline tests alone |
| Pipeline test failure with "expected X, got Y" diff but the diff looks identical | Hidden Unicode characters (non-breaking spaces, BOM) from editor auto-format | Run `cat -A file.smt` to reveal hidden chars; add `.editorconfig` with `charset = utf-8` |
| Tests pass locally but fail in CI | CI uses GCC 13, local uses GCC 11 — different warning flags become errors | Test with `make ci-build` which replicates CI flags |
```

**When nothing obvious went wrong:** re-read for implicit constraints — things that weren't problems only because the agent happened to do them right. The absence of errors doesn't mean absence of pitfalls. Example: if the agent always ran `make clean` before `make`, that's an implicit constraint worth documenting as a rule.

Do NOT manufacture pitfalls. If the session was genuinely straightforward, omit the section.

### 2h. Setup & Dependencies

Anything needed before the workflow is usable. Only include what was actually done in the session — don't invent prerequisites.

```markdown
## Setup

```bash
# System dependencies
sudo apt install build-essential libgc-dev

# Clone and build
git clone https://github.com/user/smelt
cd smelt
make
```
```

## Phase 3: Synthesize the SKILL.md

### 3a. Scaffold First, Then Fill

**Do not write the skill top-to-bottom in one go.** Assemble progressively:

1. **Create an empty skeleton** with every applicable section header and `TBD` placeholders:
```markdown
---
name: <name>
description: >
  TBD
---

# <Title>

## Quick Reference
TBD

## Workflows
TBD

## Rules
TBD

## Project Map
TBD

## Pitfalls
TBD

## Setup
TBD
```

2. **Fill each section from the extraction artifacts** (Pass 1, Pass 2, Pass 3), one at a time.
3. **Mark sections complete** as you go — this prevents dropped sections.
4. **Write the description LAST** — after all content is filled, the trigger conditions will be obvious from what's actually in the skill.
5. If a section has no content after extraction, delete the `TBD` placeholder and the header. Don't leave `TBD` in the final output.

### Minimal Viable Skill

A skill does not need every section to be useful. **Ship it if you have even ONE of these:**

| If you have... | You have a viable skill |
|----------------|------------------------|
| Quick Reference + one pitfall | Yes — the agent can run commands and avoid one known trap |
| One workflow + Project Map | Yes — the agent can follow a process in the right files |
| Rules + Quick Reference | Yes — the agent knows conventions and can execute |
| Pitfalls only | Yes — a "don't do these things" skill prevents damage |
| Setup + Quick Reference | Yes — the agent can bootstrap and run |

**Useful beats complete.** A 15-line skill with one pitfall and one command is more valuable than a 200-line draft that never gets finished because you're trying to fill every section perfectly.

### Output Format

```markdown
---
name: <kebab-case-name>
description: >
  Use when [working on / debugging / building] <domain>.
  Triggers: <natural-language phrases>.
  Does NOT apply to: <false positives>.
---

# <Human-Readable Title>

## Quick Reference

<copy-paste-ready commands, organized by category. Include WHEN and WHY, not just WHAT.>

## Workflows

<step-by-step processes with decision points. Numbered.>

## Rules

<non-obvious conventions and constraints. Each rule explains WHY.>

## Project Map / Key Files

<file tree and table of important files. Include edit frequency and purpose.>

## Pitfalls

<table: Trap → Root Cause → Fix. If nothing went wrong, omit this section — don't fabricate.>

## Setup

<first-time setup instructions. Only if the session included setup steps.>
```

### Rules for the Output

1. **No fluff.** Every line actionable. If a future agent reads it, they should know exactly what to do.
2. **Copy-paste ready.** Commands in code blocks that work as-is (or with obvious placeholders like `<file>`).
3. **Concrete over abstract.** "Run `make test-quick` to verify after parser changes" not "Run tests to verify."
4. **Capture WHY, not just WHAT.** "Use `make clean && make` because the dependency tracker is unreliable" not just "Use `make clean`."
5. **The description IS the trigger with negative filtering.** The "Does NOT apply to" line prevents the skill from loading on adjacent but unrelated tasks.
6. **Name must match directory.** If the user creates `~/.pi/agent/skills/my-project/`, the name must be `my-project`.
7. **Omit empty sections.** Don't write "## Pitfalls — none found." Just skip it.
8. **Iterated > first-attempt.** The final working version is the canonical command; failed attempts inform pitfalls.

## Phase 4: Deliver

### 4a. Show the SKILL.md

Present the full SKILL.md content to the user as the primary output. Use an artifact for large skills.

### 4b. Validate Before Writing

**Before any `write` call, validate the SKILL.md against the Agent Skills spec.** A broken frontmatter poisons ALL skill loading on reload — not just this skill.

```bash
python3 -c '
import re, sys, yaml
with open(sys.argv[1]) as f:
    content = f.read()

match = re.match(r"^---\n(.*?)\n---", content, re.DOTALL)
if not match:
    print("FATAL: No YAML frontmatter found")
    sys.exit(1)

fm = yaml.safe_load(match.group(1))
name = fm.get("name", "")
desc = fm.get("description", "")
errors = []

if not re.match(r"^[a-z0-9]([a-z0-9-]*[a-z0-9])?$", name):
    errors.append(f"Invalid name: {name}")
if len(desc) > 1024:
    errors.append(f"Description too long: {len(desc)} chars (limit 1024)")
if len(name) > 64:
    errors.append(f"Name too long: {len(name)} chars (limit 64)")
if not desc:
    errors.append("Description is empty")

if errors:
    for e in errors:
        print(f"ERROR: {e}")
    sys.exit(1)

print(f"OK: name={name}, desc={len(desc)} chars")
' /tmp/skill-draft.md
```

Pipe the draft to a temp file first, validate it, then write to the actual path. If validation fails, surface the errors — do NOT write to `~/.pi/agent/skills/`.

### 4c. Determine the write path

The skill goes in `~/.pi/agent/skills/<name>/SKILL.md` (global) or `.pi/skills/<name>/SKILL.md` (project-local). Global is the default unless the user specifies otherwise.

### 4d. Handle Existing Skill — Merge, Don't Overwrite

**Check if a skill with the same name already exists** (substitute the actual skill name from Phase 2a — `<name>` is a placeholder, not a literal path):

```bash
# Replace "my-skill" with the actual derived name
ls ~/.pi/agent/skills/my-skill/SKILL.md 2>/dev/null && echo "EXISTS" || echo "NEW"
```

If the skill already exists:

1. **Read the existing SKILL.md** into context.
2. **Diff the sections:**
   - **Quick Reference:** If the new session adds commands not in the existing skill, append them.
   - **Pitfalls:** **Append-only by default.** Never remove an existing pitfall unless the new session explicitly proves it's no longer relevant (e.g., "that bug was in v1.2, fixed in v2.0"). New pitfalls append to the table.
   - **Rules:** Merge. If a new rule contradicts an old one, flag it for the user — don't silently overwrite.
   - **Workflows:** If it's a new workflow, append. If it's the same workflow with refinements, merge into the existing one — keep the most detailed version. **If the new session's workflow contradicts an existing one step-for-step** (e.g., old workflow has 6 steps, new one has 4 and skips a known-critical step like IAM verification), do NOT merge and do NOT silently overwrite. Flag the conflict explicitly: "Existing skill has a Deploy workflow with 6 steps; this session's workflow has 4 steps and skips the IAM check. Which is current?" Let the user decide.
   - **Project Map:** Merge file entries. Old files might still exist.
   - **Setup:** Merge — dependencies are additive.
3. **Show a diff summary** to the user: "Existing skill has 3 pitfalls, this session adds 2: ..."
4. **Ask before writing** (use `ask_cards` for the confirmation). Never silently overwrite.

If the skill does NOT exist, write it directly (or ask for confirmation if the user didn't explicitly say "write it").

### 4e. Tell the user to reload

After writing, remind: `/reload` to pick up the new skill. The skill won't be active in the current session until reloaded.

### 4f. Clean Up Temp Files

Remove extraction artifacts and temp files created during the process:
```bash
rm -f /tmp/session-extract*.txt /tmp/skill-draft.md
```

## Anti-Patterns for This Skill

| Don't | Do |
|-------|-----|
| Dump the entire session transcript into the skill | Extract and categorize by importance signal |
| Write vague "helps with X" descriptions | Use the template with triggers and negative filters |
| Invent content to fill gaps | Omit empty sections — useful beats complete |
| Use placeholder names like `my-skill` | Derive name from session domain |
| Overwrite an existing skill silently | Diff, merge, show user what changed |
| Skip pitfalls because "nothing went wrong" | Re-read for implicit constraints — things that went right by accident |
| Truncate session extraction at 500 chars | Read full content; pipe to artifact for large sessions |
| Suppress JSONL parse errors with `2>/dev/null` | Surface them — corrupt lines might hold key insights |
| Treat all content as equal weight | Apply the importance signal table — iterated > first-attempt |
| Merge unrelated domains into one skill | Split if trigger descriptions wouldn't overlap |

## Complete Example

Below is an example output from a session developing a Rust-based CLI tool for managing AWS Lambda deployments:

---

```markdown
---
name: lambda-deploy
description: >
  Use when deploying, updating, or debugging AWS Lambda functions via the lambda-cli tool.
  Triggers: "lambda", "deploy lambda", "AWS Lambda", "lambda update", "lambda logs",
  "lambda invoke", "serverless deploy".
  Does NOT apply to: general AWS CLI usage (S3, EC2, IAM), Terraform-based Lambda management,
  Lambda@Edge or CloudFront functions, container-image Lambda deployments (this tool is
  zip-only).
---

# Lambda Deploy CLI

## Quick Reference

### Build and Package
```bash
# Build in release mode (ALWAYS release — debug builds hit Lambda's 250MB unzipped limit)
cargo build --release --target x86_64-unknown-linux-musl
# Strip debug symbols (saves ~40MB)
strip target/x86_64-unknown-linux-musl/release/function
# Package
zip -j function.zip target/x86_64-unknown-linux-musl/release/function
```

### Deploy
```bash
lambda-cli deploy --name my-function --zip function.zip --runtime provided.al2
```

### Debugging
```bash
# Tail logs (30 min window, streaming)
lambda-cli logs --name my-function --tail

# Invoke with test payload
lambda-cli invoke --name my-function --payload '{"key": "value"}'

# Check cold start timing
lambda-cli logs --name my-function --tail | grep "REPORT RequestId" | grep "Init Duration"
```

## Workflows

### First Deployment of a New Function

1. **Verify IAM role exists** — `lambda-cli roles list | grep my-function-role`. If missing, create it FIRST. Deploying without the role succeeds but the function fails at invocation with an opaque permissions error that takes 20 minutes to diagnose.
2. **Build with musl target** — `cargo build --release --target x86_64-unknown-linux-musl`. The `provided.al2` runtime requires static linking. A glibc build will fail with "cannot execute binary file."
3. **Strip + zip** — `strip target/.../function && zip -j function.zip target/.../function`. If you forget `-j` (junk paths), the zip retains directory structure inside and Lambda can't find the bootstrap.
4. **Deploy** — `lambda-cli deploy --name my-function --zip function.zip --runtime provided.al2`
5. **Verify cold start** — `lambda-cli invoke --name my-function --payload '{"test": true}' && lambda-cli logs --name my-function --tail | grep "Init Duration"`. Init Duration should be <200ms for Rust. If >1s, the binary is probably not stripped.
6. **Set concurrency** — `lambda-cli concurrency --name my-function --reserved 10`. Without this, a traffic spike can exhaust account-level concurrency and throttle unrelated functions.

### Updating an Existing Function

1. `cargo build --release --target x86_64-unknown-linux-musl`
2. `strip target/.../function && zip -j function.zip target/.../function`
3. `lambda-cli deploy --name my-function --zip function.zip` (runtime is cached, don't need to respecify)
4. **Wait for the update to propagate** — `lambda-cli wait --name my-function` (the deploy command returns before the update is live; invoking immediately hits the old version)
5. `lambda-cli invoke --name my-function --payload '{"smoke": true}'` to verify the new code is running

## Rules

1. **Always build with musl.** The `provided.al2` runtime runs on Amazon Linux 2 with musl. A glibc binary will fail silently at invocation with no useful error.
2. **Always strip before zipping.** Rust debug symbols add ~40MB. Lambda has a 250MB unzipped limit. A debug build + debug symbols can easily exceed it.
3. **Always `zip -j` (junk paths).** Lambda looks for `bootstrap` at the zip root. Directory nesting from `zip function.zip target/x86_64-unknown-linux-musl/release/function` puts it at a path Lambda won't find.
4. **Set reserved concurrency on every production function.** Without it, one function's traffic spike starves all other functions in the account.
5. **Never rename a function in production.** Lambda functions are immutable by name. Renaming = delete + create = cold starts + DNS propagation + lost invocation history. Use aliases for versioning.

## Project Map

```
lambda-cli/
├── src/
│   ├── main.rs           # CLI argument parsing (clap); dispatch to subcommands
│   ├── deploy.rs         # Zip upload, CreateFunction/UpdateFunctionCode API calls
│   ├── logs.rs           # CloudWatch Logs tail with filtering
│   └── aws_client.rs     # Shared STS/SigV4 signing — do NOT touch unless AWS API changes
├── tests/
│   └── integration/      # End-to-end tests (require AWS credentials, skipped in CI)
└── Cargo.toml
```

## Pitfalls

| Trap | Root Cause | Fix |
|------|-----------|-----|
| "cannot execute binary file" at invocation | Built with glibc target instead of musl | Use `--target x86_64-unknown-linux-musl`. Add `.cargo/config.toml` with `[build] target = "x86_64-unknown-linux-musl"` to make it the default. |
| Deploy succeeds but "bootstrap not found" in logs | Forgot `zip -j` flag — directory structure preserved inside zip | Always `zip -j`. Add a pre-deploy check: `unzip -l function.zip | grep bootstrap` |
| Init Duration >1 second | Binary not stripped — Lambda unpacking a 60MB zip | Strip religiously. Add a build script that fails if binary >15MB. |
| Invoke returns old code after deploy | `deploy` returns before update propagates; hitting the old version during the rollout window | Always run `lambda-cli wait --name <fn>` after deploy, or use the `--wait` flag on deploy. |
| "Rate exceeded" on unrelated functions during traffic spike | No reserved concurrency — one function consumes the entire account pool | Set reserved concurrency on every function. 5 minimum for low-traffic, 10+ for production. |
| `lambda-cli` hangs on deploy for large zips | S3 multipart upload timeout — default is 30s, large zips take longer | Set `--upload-timeout 300` for zips >20MB. Or use S3-first deploy: `--s3-bucket my-bucket` to bypass direct upload. |

## Setup

```bash
# Install Rust (if not already)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Add musl target
rustup target add x86_64-unknown-linux-musl

# Install musl toolchain (system linker)
sudo apt install musl-tools

# Clone and build
git clone https://github.com/user/lambda-cli
cd lambda-cli
cargo build --release

# Configure AWS credentials (if not already)
aws configure
# Requires: lambda:CreateFunction, lambda:UpdateFunctionCode, lambda:InvokeFunction,
#           logs:DescribeLogStreams, logs:GetLogEvents
```
```
