# Settings SPA audit — baseline (2026-08-19)

**Scope:** Hub Settings cabinet + `ui.json` persistency + category IA.  
**Product goal:** Settings as a real **SPA surface** — clear categories, complete options, honest persist, no tetris-list debt.

## Current architecture

| Piece | Path | Role |
|-------|------|------|
| Row table | `src/ui/model/settings_table.hpp` | Flat `kSettingsRows[]` Heads + Items |
| Draw | `MainScene::drawSettings` in `hub_draw.hpp` | List paint + value strings |
| Keys | `hub_keys.hpp` | j/k step, ←→ carousel, enter toggle |
| Persist | `src/ui/model/ui_prefs.hpp` | `~/.config/cortex-mk3/ui.json` load/save/atexit |
| Shadow | `UiPrefShadow` | Bridge model ↔ disk |

## Categories today (IA)

```
LOOK     THEME · FIELD · SHADER
CHAT     THOUGHTS · TRUNCATE · INPUT FMT · OUTPUT FMT · RAW · CHAT FIELD · FOOTER PANE · LIVE FOLLOW
CHROME   ZEN · NAV PILL · PILL HIDE
SESSION  CWD · REMEMBER CWD · KEEP LIVE · SESSION SCOPE
DEV      DEV MODE
```

## Gaps (overhaul drivers)

### A. Missing options (exist in prefs/model, not in Settings UI)

| Pref / model field | Settings row? | Notes |
|--------------------|---------------|-------|
| `chatBodyMode` (stream/compact/canvas) | **NO** | Ctrl-O only; prefs key `chat_body_mode` persists but no cabinet row |
| Provider / model | **NO** | Launch-time / CLI; hub has no engine picker in Settings |
| Density tier | **NO** | layout density exists elsewhere |
| Completions / keymap help | **NO** | buried in help |
| Compaction / history caps | **NO** | agent.yml only |
| Fallback provider display | **NO** | runtime only |

### B. Category / labeling debt

- **INPUT/OUTPUT FMT** = action/result body render modes — names are opaque.
- **FIELD** vs **CHAT FIELD** vs **SHADER** — three “background” knobs; mental model unclear.
- **PILL HIDE** is a carousel (ms) not a toggle — label doesn’t say so.
- **SESSION SCOPE** GLOBAL/PROJECT — good, but lonely without “export path / CORTEX_HOME”.
- No **AGENT / ENGINE** category (provider, model, harness, max_iterations surface).
- No **PROTOCOL / HARNESS** category (dev dumps already under DEV only).

### C. Persistency audit

| Check | Status |
|-------|--------|
| Path XDG `ui.json` | OK |
| load on boot → applyUiPrefsToModel | OK |
| capture on change + atexit flush | OK |
| `chat_body_mode` load/save | OK (prefs) |
| Settings row for body mode | **MISSING** |
| Schema version field | **MISSING** — hard to migrate |
| Atomic write (tmp+rename) | **VERIFY** — hand JSON ostringstream |
| Corrupt file recovery | soft defaults via jsonGet* fallbacks |
| Multi-instance races | last-writer-wins |
| settingsFocus / scroll persisted | **NO** |

### D. SPA / UX craft debt

- Flat list, not multi-pane SPA (no left rail category jump, no detail panel).
- No search/filter for options.
- No “modified / restart required” badges.
- Value column can truncate long CWD without expand.
- `settingsOptionCount = 32` legacy constant may drift from `kSettingsRowN`.
- Tetris aesthetic target stated; implementation is still a plain list.

### E. Interaction contract (keep)

- Headers non-focusable — good.
- Carousel vs toggle via `settingsIsCarousel` — good.
- CWD `e` edit — good.

## Target SPA shape (proposal)

```
┌────────────┬──────────────────────────────────────┐
│ CATEGORIES │  OPTIONS (focused category)          │
│ LOOK       │  label          value      hint      │
│ CHAT   ●   │  BODY VIEW      canvas     ← →       │
│ ENGINE     │  …                                   │
│ SESSION    │                                      │
│ DEV        │  detail / help strip under focus     │
└────────────┴──────────────────────────────────────┘
```

**New categories (draft):**

1. **LOOK** — theme, field, shader  
2. **CHAT** — thoughts, truncate, body view (stream/compact/canvas), footer pane, follow, raw, body fmts  
3. **ENGINE** — provider, model, fallback (read-only or picker), thinking  
4. **SESSION** — cwd, remember, keep live, scope  
5. **DEV** — dev mode, dump path hint, verbose  

## Slice plan

| ID | Slice | Acceptance |
|----|-------|------------|
| **T-SET-IA** | Re-IA categories + rename opaque labels | Spec in settings_table + draw labels |
| **T-SET-BODYVIEW** | Settings row for chat body mode | Round-trip ui.json + UI |
| **T-SET-PERSIST-V** | `ui_schema_version` + atomic write | Survive crash mid-write |
| **T-SET-SPA** | Two-pane category rail + option list | Keyboard: h/l or tab categories |
| **T-SET-ENGINE** | ENGINE category (provider/model surface) | At least display + deep-link to picker |
| **T-SET-AUDIT-TEST** | settingsOptionCount sync / tests | unit test row table |

## Non-goals (this board)

- Full pi-parity settings DAG  
- Remote sync of prefs  
- Mouse-only redesign  

## Evidence paths

- `settings_table.hpp` — source of truth for rows  
- `ui_prefs.hpp` — persist  
- `hub_draw.hpp` `drawSettings` — paint  
- Operator: `~/.config/cortex-mk3/ui.json`
