# Home · Sessions · Body views overhaul

**Status:** plan + wave-0 code (graph name kill, compact correctness)  
**Branch tip target:** `feat/inkcell-agentshell` / master  
**Operator ask:** big move — home + sessions UI/UX/QOL; **remove graph view** (not the canvas they asked for); fix compact bugs; then raise craft.

---

## 0. Glossary (do not confuse)

| Name | What it is | Keep? |
|------|------------|-------|
| **Stream** | Default chat body — full timeline projection | yes |
| **Compact** | One-line-per-row operator index over `TimelineRow*` | yes — fix then raise |
| **Canvas** (chat Ctrl-O) | Protocol flow spine: ACT→OK cards from timeline | yes — not “graph” |
| **Graph** (chat) | Naming debt / alias of canvas + stale comments | **REMOVE name** |
| **Workflow canvas** | Manifests/Workflows page infinite canvas | separate product; keep |
| **Theme graphite** | Color pack | keep |

Chat body modes after cleanup: **`stream · compact · canvas`** only. No “graph” in UI, footer, prefs comments, or keyhints.

---

## 1. Diagnosis — Home

**Today (`drawHome`):** hero name + LIVE/READY · KPI tiles · RUNTIME/LOADOUT columns · RECENT sessions · one footer line.

| Pain | Detail |
|------|--------|
| KPI density | Five tiles (REGISTRY/AGENTS/SESSIONS/TOOLS/SUBS) — true counts but low action |
| RECENT not focusable | Decorative list; cannot j/k open without leaving Home |
| Live truth weak | “turn continues on Settings…” — not NOW/phase |
| No primary CTA chrome | enter works but page doesn’t feel like a launchpad |
| No empty/error hierarchy | launchError is one red line at bottom |
| Field wallpaper tax | OK at rest; competes with content on short terms |

**North star — Home as command surface**

```
┌─ CORTEX MK3 · agent · ● LIVE ──────────────── engine/model ─┐
│  NOW  tool in flight · grep #g1 · 0:42                     │
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐                               │
│  │OPEN│ │NEW │ │SESS│ │REG │   primary actions (focusable) │
│  └────┘ └────┘ └────┘ └────┘                               │
│  RECENT (focusable, ↵ resume)                              │
│  ▸ sess-…  coder  · 12t  · 2m ago  · LIVE                  │
│    sess-…  default · 3t   · yesterday                      │
│  RUNTIME strip (one line): cwd · harness · hist            │
│  enter chat · j/k recent · n new · s all sessions          │
└────────────────────────────────────────────────────────────┘
```

**QOL**

- Focus ring on RECENT + action tiles (not only Sessions page)
- Live session always pinned top of RECENT
- Relative time (“2m ago”) not only ISO
- Enter on empty = new chat; Enter on recent = resume
- `/` search jumps Sessions with query seeded
- Optional: last dump path when DEV MODE

---

## 2. Diagnosis — Sessions

**Today (`drawSessions`):** header KPI · key encyclopedia line · column headers · scroll list · LIVE chip · n/d/e/x.

| Pain | Detail |
|------|--------|
| Key encyclopedia | Full bind list on row 2 steals vertical space |
| Columns fragile | Hard-coded x offsets break <64 cols |
| Scope weak | GLOBAL/PROJECT only in Settings, not page chrome |
| No preview | No teaser of last user/agent line |
| Delete UX | Guard notice only; no type-to-confirm craft |
| Search | `/` exists but empty/filter chrome thin |
| Multi-select | None (export/kill batch) |
| Sort | Disk order only |

**North star — Sessions as session OS**

```
┌─ SESSIONS · 12 disk · 1 live · scope PROJECT ── /filter ─┐
│  [All] [Live] [This agent] [This cwd]     sort ▾ updated │
│  STATE  AGENT      TURNS  UPDATED   ID                   │
│  ●LIVE  coder        12   2m ago    …abc123   ▸          │
│  ○     default        3   yesterday …def456              │
│  ─ preview ────────────────────────────────────────────  │
│  YOU  light scout…                                       │
│  OUT  inkcell is a C++17…                                │
│  n new · ↵ open · d delete · e export · x kill · tab scp │
└──────────────────────────────────────────────────────────┘
```

**QOL**

- Filter chips: All / Live / Agent / Cwd  
- Sort: updated · created · turns · name  
- Preview pane (last 3 timeline-ish lines from session file)  
- Soft key footer (one line), full binds in `?`  
- Confirm delete with type-confirm for active session  
- Export path shown in notice  
- Relative timestamps  
- Width-adaptive columns (collapse ID first)

---

## 3. Body views — remove graph name · fix compact · raise canvas

### 3.1 Remove “graph” (chat)

- Delete `drawTranscriptGraph` alias or keep private inline → `drawTranscriptCanvas` only  
- Comments/prefs: `0 stream · 1 compact · 2 canvas`  
- agent_scene keyhint: no “graph”  
- Settings BODY VIEW already says stream/compact/canvas — verify  
- Do **not** touch workflow canvas

### 3.2 Compact bugs (P0 code)

| Bug | Fix |
|-----|-----|
| `contentH` ignores header row | `contentH = total + 1` (title); scroll math uses `vis` not full `body.h` |
| Selection vs filter | Map `selectedRow` through `idx[]`; highlight by filtered index |
| Double teaser on results | Title already has meta; teaser only if body adds new info |
| Follow-bottom + header | `maxOff = max(0, total - vis)` with `vis = h - 1` |
| Empty / no rows | Clear empty state; if stream has lines but no rows, offer “rebuild” notice |
| j/k while compact | Ensure timeline focus still moves `selectedBlock` and scroll snaps to selection |
| Status noise | Collapse consecutive SYS if same prefix (optional QOL) |
| Width | `fit_left` after building line; tag fixed 4 cols |

### 3.3 Compact craft (P1)

- Pair ACT+OK on one visual line when adjacent same id (`RD list #a1 → OK 12ms`)  
- Kind colors already via rails — keep  
- Sticky mini-header: counts `12 rows · 3 act · 1 open`  
- Enter on row = expand teaser / jump stream focus  
- `/` filter kinds (act/ok/you/sys)

### 3.4 Canvas craft (P1 — chat body, not workflow)

- Keep spine + ACT→OK edges  
- Fix scroll contentH similarly  
- Optional: only show Action/Result/User/Response nodes (drop pure thoughts when thoughts off — already)  
- Do not invent mouse until inkcell scene mouse path is intentional  

---

## 4. Execution waves

### Wave 0 — now (this commit family)
1. Kill graph naming; canvas-only mode 2  
2. Compact scroll/header/selection correctness  
3. Plan doc (this file) + TICKETS board  

### Wave 1 — Home launchpad
1. Focusable RECENT + pin live  
2. Action strip OPEN/NEW/SESS/REG  
3. NOW line from same phase source as chat footer  
4. Relative time helper  

### Wave 2 — Sessions OS
1. Adaptive columns + filter chips  
2. Preview strip from session JSON (last user/agent)  
3. Soft footer; sort updated  
4. Delete confirm craft  

### Wave 3 — Compact/canvas craft
1. ACT→OK pairing in compact  
2. Canvas contentH + selection sync  
3. Live TUI verify Ctrl-O ×3  

### Wave 4 — Polish / perf
1. Home short-term layout (h < 20)  
2. Sessions virtualize if >200  
3. Snapshot tests for home/sessions empty+populated  

---

## 5. Acceptance

| Surface | Pass |
|---------|------|
| Ctrl-O | Cycles **stream → compact → canvas** only; no “graph” string in UI |
| Compact | j/k selection visible; scroll doesn’t jump under header; no double teaser; follow-bottom correct |
| Home | Can open recent with j/k+enter without visiting Sessions; live pinned |
| Sessions | Readable at 80 and 120 cols; preview shows last beat; delete can’t silent-fail active |
| No regress | `test-ui-model` `test-chat-scene`; headless list→final |

---

## 6. Non-goals
- Replacing workflow infinite canvas  
- Full mouse SPA on hub (unless inkcell path lands)  
- Settings SPA (separate board T-SET-*)  

---

## 7. Key files

| File | Role |
|------|------|
| `src/ui/scenes/hub_draw.hpp` | drawHome / drawSessions |
| `src/ui/scenes/hub_keys.hpp` | section keys |
| `src/ui/scenes/hub_session_ops.hpp` | resume/delete/export |
| `src/ui/chat/chat_body_views.hpp` | compact + canvas |
| `src/ui/chat/chat_view.hpp` | dispatch body modes |
| `src/ui/scenes/agent_scene.hpp` | Ctrl-O |
| `src/ui/model/dashboard_model.hpp` | sessions list state |

GODSPEED.
