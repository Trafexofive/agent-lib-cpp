# CORTEX MK3

**Production agent runtime + inkcell-native TUI.**  
XML protocol harness, multi-provider LLM backends, sandboxed tools, sub-agents, sessions — daily-driven as `cortex-mk3`.

```
  CORTEX MK3
  protocol · tools · sub-agents · inkcell glass
```

| | |
|--|--|
| **Binary** | `cortex-mk3` (CLI + experimental/inkcell TUI) |
| **Protocol** | MK3 Agent Protocol v3 — [`docs/protocol/CANON.md`](docs/protocol/CANON.md) |
| **UI** | inkcell App (`src/ui/`) — hub + chat instrument |
| **Version** | v3.1.x (`cortex-mk3 version`) |

---

## Quick start

```bash
# build + install (user bin)
make -j"$(nproc)" install
# → ~/.local/bin/cortex-mk3

# interactive TUI (default inkcell / experimental)
cortex-mk3
cortex-mk3 -m default                    # agent under manifests/agents/
cortex-mk3 --provider opencode-go --model deepseek-v4-flash

# headless one-shot (for scripts / dumps)
CORTEX_DEV_MODE=1 cortex-mk3 --ephemeral --no-session \
  --provider opencode-go --model deepseek-v4-flash \
  run -p 'list path=. max_entries=20'
# dumps → ~/.cortex/dev/<session>/  (history.md iterations.md protocol.md raw.md)
```

**Providers:** deepseek · openrouter · xai / x-ai · openai-codex · groq · zen · opencode / opencode-go · together · fireworks · …

Keys via env (`DEEPSEEK_API_KEY`, `OPENCODE_API_KEY`, `OPENROUTER_API_KEY`, …) or pi auth store (`~/.pi/agent/auth.json` for xAI OAuth).

---

## What it is

CORTEX is a **containment harness for LLMs**, not a chat wrapper:

1. **Protocol** — model emits `<action>` / `<thought>` / `<response final="true">`; runtime injects `<result>`. Bare prose does not finish a turn.
2. **Tools & sub-agents** — exec, fs, grep, list, tree, ask_tool, `wait`/`join`, delegated agents (`type="agent"`).
3. **inkcell TUI** — hub (home / sessions / manifests / tools / settings) + chat with live footer instrument, Ctrl-O body views (stream · compact · canvas).
4. **Sessions & dumps** — JSON sessions under `~/.cortex/`; dev dumps for autopsy when `CORTEX_DEV_MODE=1` or Settings → dev mode / `/export-dump`.

Product UI lives in **`src/ui/`** (inkcell). Legacy `src/tui` is oracle-only (`--tui legacy` if present).

---

## Layout

```
src/
  core/         Agent loop, prompts, manifests, session dump, sub-agents
  protocol/     Streaming XML parser
  providers/    OpenAI-compatible clients (DeepSeek, OpenRouter, xAI, OpenCode, …)
  tools/        Built-ins (exec, list, grep, fs_*, sleep, ask_tool, …)
  feeds/        Context feeds
  session/      Persistence
  ui/           inkcell product (app · scenes · chat · model · theme · gfx)
  cli/          Flags, run, serve, completions

manifests/
  agents/       Launchable agents (default, coder, …)
  harness/      Protocol harness text (CANON projection)
  tools/        Script tools (e.g. tree)
  system/ persona/

docs/
  protocol/CANON.md
  CONTINUITY/           session checkpoints for long grinds
  AUDITS/REPORTS/       dump autopsies
  tui-qol/              chat/footer design notes
```

---

## Protocol (short)

Authority: **[`docs/protocol/CANON.md`](docs/protocol/CANON.md)**.

```xml
<thought>…</thought>
<action type="tool" name="list" id="a1" mode="sync">{"path":"src","max_entries":30}</action>
<!-- runtime --><result id="a1" ok="true" ms="1.2" bytes="400">…full body…</result>
<response final="true">…operator-facing answer…</response>
```

| Rule | |
|------|--|
| Final | only `<response final="true">` |
| Results | runtime-only; model never forges `<result>` |
| History SoT | tool/sub-agent bodies stay **full** in history (UI may truncate paint) |
| Sub-agents | prefer `mode="sync"`; hollow `{}` agent bodies are rejected |
| Join | `wait` / `join` tool — not `sleep` as await |

---

## TUI cheatsheet

| | |
|--|--|
| **Hub** | `m` dashboard · section keys on home · Enter launch |
| **Chat** | Enter send · S-Enter newline · Esc composer↔timeline |
| **Cancel** | Ctrl-C stop turn (then quit if idle) |
| **Views** | **Ctrl-O** stream → compact → canvas |
| **Footer pane** | Ctrl-F live · sess · eng |
| **Dev dumps** | Settings → DEV MODE, or `CORTEX_DEV_MODE=1` |

Footer (live): RUNNING badge · NOW line · context pressure bar · turn/iter/tools/stream/hist · last/open · panes.

---

## Build / test / install

```bash
make -j"$(nproc)"              # cortex-mk3
make install                   # ~/.local/bin/cortex-mk3
make test-parser test-ui-model test-chat-scene
make test-protocol             # fixture suite when available
```

inkcell is a sibling checkout (`../inkcell` or `INKCELL_ROOT`).

---

## Brand

| Token | Use |
|-------|-----|
| **CORTEX** | product family |
| **MK3** | this runtime generation |
| **cortex-mk3** | CLI / binary name |
| Glass | inkcell terminal UI — graphite / neon themes |

Hub bar: `CORTEX` + cyan `MK3`. Chat assistant label = live agent name (not a frozen brand string).

---

## Continuity & dumps

Long sessions leave checkpoints under `docs/CONTINUITY/`.  
Operator dump autopsies: `docs/AUDITS/REPORTS/`.

```bash
# after a headless turn with CORTEX_DEV_MODE=1
ls -td ~/.cortex/dev/*/ | head -1
# history.md · iterations.md · protocol.md · raw.md
```

---

## Status

Active branch work targets a **daily-driver** harness: honest protocol export, full result SoT in history, inkcell chat instrument, sub-agent join. See latest `docs/CONTINUITY/*-checkpoint.md` for open ROI.

**License / contrib:** see `LICENSE`, `CONTRIBUTING.md` if present.

```
GODSPEED
```
