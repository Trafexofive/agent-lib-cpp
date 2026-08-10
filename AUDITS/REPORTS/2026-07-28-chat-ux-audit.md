# Chat UX Audit Report — Cortex-MK3 inkcell Port

**Branch:** `feat/inkcell-agentshell` | **Files Audited:** `src/ui/chat/*.hpp`, `src/ui/scenes/agent_scene.hpp`, `src/ui/scenes/main_scene.hpp`, `src/ui/model/inkcell_app_model.hpp`, `src/ui/views/timeline_view.hpp`, `src/ui/layout/sbtui_layout.hpp`, `src/ui/theme/cortex_theme.hpp`

---

## Executive Summary

The chat surface is **functionally complete but UX-unpolished**. Recent fixes addressed the "big three" (scroll, contiguous transcript, agent-name labels). What remains are ~20 concrete paper cuts across chrome, composer, transcript, subagent drill-down, help, ask dialog, notices, selection, color/contrast, and long-output handling. Every item below is a **localized fix** — no architecture changes.

---

## 1. Header / Status / Prompt Line Chrome (High Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 1 | `chat_view.hpp:247-260` `drawHeader` | `m.title` hardcoded to `"CORTEX MK3"` | Still shows CORTEX sentinel in header despite agentName being available in model | Use `m.agentName.empty() ? m.title : m.agentName` for left label |
| 2 | `chat_view.hpp:247-260` `drawHeader` | Right side shows `provider/model · session-suffix` | No visual hierarchy; provider/model runs together with session id; no agent identity | Group: `agentName · provider/model · session-suffix` with separators ` · ` |
| 3 | `chat_view.hpp:218-245` `drawStatusLine` | Right side switches between live metrics (running) and `m.hint` (idle) | Hint text is generic; when idle shows "ready" but no last-turn summary; no elapsed time for completed turn | When idle: show `last <status> <elapsed> · <tokens>b` using `lastTurnElapsedMs` + `tokenBytes` from model |
| 4 | `chat_view.hpp:218-245` `drawStatusLine` | Running state shows `pending N · X actions · Y results · Zb` | "pending" count = in-flight actions; unclear distinction from actions/results; no live elapsed timer | Add live elapsed `MM:SS` since `turnStartMs`; label as `live` vs `last` |
| 5 | `chat_view.hpp:262-280` `drawPromptLine` | Prompt shows `› input█` when focused, `  input` when unfocused | No visual distinction between "composer empty + focused" (shows `█`) vs "composer has text + unfocused"; cursor block `█` is raw, no blink, no I-beam hint | Show `› ` prefix only when focused; when unfocused + non-empty show `  ` prefix; consider subtle cursor style `│` when focused + non-empty |
| 6 | `chat_view.hpp:262-280` `drawPromptLine` | Truncates at `row.w` with `truncate()` | Long inputs silently truncate; no indication of hidden text; no horizontal scroll | Add `…` ellipsis on left when truncated; or reserve 2 cols for scroll hint `◀▶` |
| 7 | `agent_scene.hpp:145-160` (draw) | `vm.mode` = `"FULL" \| "FULL+THOUGHTS" \| "RAW"` | Mode label is cryptic; `FULL` means nothing to users; no toggle hint in status line when idle | Rename: `FULL` → `NORMAL`, `FULL+THOUGHTS` → `THOUGHTS`, `RAW` → `RAW`; show toggle keys in hint: `t/r` |

---

## 2. Composer Behavior (High Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 8 | `inkcell_app_model.hpp:780-800` `submitComposer` | Trims trailing whitespace/newlines, then leading whitespace | **Eats intentional leading whitespace** (e.g. code blocks, markdown fences) | Only trim trailing; preserve leading |
| 9 | `agent_scene.hpp:90-110` `on_key` (composer focus) | `Tab` = slash command completion; no `Shift+Tab`; no `Ctrl+U`/`Ctrl+K`/`Ctrl+W` | Missing standard line-editing bindings; Tab completion steals `Tab` from potential multi-line input | Add `Ctrl+U` (kill line), `Ctrl+K` (kill to end), `Ctrl+W` (kill word), `Alt+Backspace` (kill word back); keep `Tab` for completion only when prefix starts with `/` |
| 10 | `inkcell/widgets/textarea.hpp` (inkcell) | `TextAreaState` supports multi-line but chat uses single-line | No way to input multi-line prompts (code, markdown, pastes) | Enable multi-line composer: `Ctrl+Enter` = send, `Enter` = newline; show line count hint |
| 11 | `agent_scene.hpp:90-110` | `ArrowUp/Down` = history when composer focused | History navigation replaces composer content **without confirmation**; no way to recover drafted text if accidental | Store draft in `promptHistoryDraft` (already exists) but restore on `Esc` or when leaving history bounds |
| 12 | `agent_scene.hpp` | Paste handling = raw terminal paste | Large pastes flood stdin; no bracketed paste detection; no confirmation for >10 lines | Detect bracketed paste (`\e[200~`/`\e[201~`); if >5 lines, show inline confirm "Paste N lines? [y/n]" |

---

## 3. Transcript Rendering: Wrap, Code Fences, Long Responses, Empty State (High Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 13 | `chat_view.hpp:140-180` `wrapTranscriptRange` | Code fences rendered as `┌─lang` / `│ line` / `└─` with hard wrap at `available-2` | **No horizontal scroll** for long code lines; hard wrap breaks readability; fence language shown but no syntax hint | Option A: horizontal scroll for fenced blocks (viewport + left/right keys). Option B: soft-wrap with `↳` continuation glyph. Minimum: increase `available` by not reserving 2 cols for `│ ` on non-fence lines |
| 14 | `chat_view.hpp:140-180` | `hardWrapUtf8` breaks on grapheme boundaries but **splits mid-word** for long tokens (URLs, hashes, base64) | Unreadable wrapped URLs/hashes | `wrapWordsLossless` already handles words > width by hard-wrapping; apply same to code fence content |
| 15 | `chat_view.hpp:340-380` `drawTranscript` | Empty transcript shows nothing (just base_bg) | No empty state message; user sees blank slate, unclear if chat works | When `displayLines.empty()`, render centered dim message: `"No conversation yet. Type a prompt and press Enter."` |
| 16 | `chat_view.hpp:340-380` | Scrollbar thumb uses `│` / `┆` at `body.right()-1` | **Overlaps last column of text** when `body.w` used for content; thumb draws on top of content | Reduce content width by 1 when scrollbar visible (`blockWidth = body.w - 1` already done but fill uses `blockWidth` while text uses `body.w`) — line 368: `surface.fill({body.x, firstY + y, blockWidth, 1}, " ", style);` should match text width |
| 17 | `chat_view.hpp:110-130` `lineStyle` / `blockStyle` | Block kinds mapped to colors; `Notice` = dim; `Raw` = dim; `Thought` = dim | **Dim fg on colored bg = low contrast** (especially graphite theme); `Thought`/`Raw`/`Notice` nearly unreadable on their dark backgrounds | For `Thought`/`Raw`/`Notice`: use `theme::text().fg` (not dim) on their backgrounds; reserve `dim` only for non-header body lines |
| 18 | `chat_view.hpp:340-380` | Selection marker `› ` prepended to header line only | When block spans multiple wrapped lines, **only first line shows selection**; body lines have no indicator | Extend `▎` left marker down all wrapped lines of selected block (already done via `fill` + `▎` at `body.x`) — but `header` flag only true for first line; fix: `header` should be true for all lines of a selected block, or add separate `selected` fill style |

---

## 4. Subagent Drill-Down: Breadcrumb, Header, Live Metrics (High Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 19 | `inkcell_app_model.hpp:350-370` `breadcrumb()` | `root / sub1 / sub2` | **No visual breadcrumb in chat header**; only in inspector pane (hidden) | Add breadcrumb line in `drawHeader` or as second header row when `!atRoot()`: `root / sub1 / sub2` with `·` separators |
| 20 | `agent_scene.hpp:145-160` `draw` | When drilled into subagent, `vm.path = model_->breadcrumb()` but header still shows root agent name + provider | **Header doesn't reflect current agent**; shows root agent's provider/model even when viewing subagent | Pass `currentAgent()->provider/model` to `vm.provider/vm.model` when `!atRoot()` |
| 21 | `inkcell_app_model.hpp:480-520` `rebuildViews` | `nestedRows` built from subagent's protocol events; `pendingOps/actionCount/resultCount` are **root totals** | **Live metrics (pending/actions/results) show root values** when viewing subagent — misleading | When `!atRoot()`, compute metrics from `currentAgent()` protocol events; or show `n/a` with subagent name |
| 22 | `agent_scene.hpp:145-160` | `vm.running` = root running; `vm.status` = root status | When drilled into subagent, **status line shows root state** (e.g. "running" even if subagent done) | `vm.running`/`vm.status`/`vm.failed` should reflect `currentAgent()` when `!atRoot()` |
| 23 | `inkcell_app_model.hpp:380-400` `enterSelected` / `goBack` | `Enter` drills in; `Backspace`/`h` goes back | **No visual transition**; instant swap disorients; no "back" hint in header when nested | Add 2-frame slide or fade; show `← Back` in header when `!atRoot()` |
| 24 | `inkcell_app_model.hpp:380-400` | Drillable rows marked with `↳ enter` in label | `↳` glyph is subtle; no highlight on drillable rows when not selected | Add `▸` prefix on drillable rows (like `> ` for user); or color `actionName` in amber when drillable |

---

## 5. Help Overlay (`drawHelpOverlay`) (Medium Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 25 | `chat_view.hpp:382-420` `drawHelpOverlay` | Static list of 15 bindings | **Missing**: multi-line composer (`Ctrl+Enter`), history recovery (`Esc`), paste confirm, theme names (`graphite`/`neon`), `/commands` catalog, copy commands (`/cp-all`, `/cp-raw`), session mgmt | Add missing bindings; group by mode (Composer / History / Drill / Global); show current theme name |
| 26 | `chat_view.hpp:382-420` | Fixed width `min(76, page.w-4)` | On wide terminals (>120), help is narrow centered box wasting space | Expand to `min(100, page.w-8)`; use two-column layout for bindings |
| 27 | `chat_view.hpp:382-420` | No context-aware filtering | Shows all bindings even when in nested view (where `i`/`g`/`u`/`d` don't apply) | Filter bindings by `model_->atRoot()` and `model_->timelineFocus` |

---

## 6. Ask Dialog / Overlay Rendering (Medium Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 28 | `chat_view.hpp:422-480` `drawAskDialog` | Renders card title, type, message, options/input | **No scroll** for long messages/options; `y >= frame.bottom()-5` hard cutoff truncates | Add scroll offset for message lines + options; show `↑↓ scroll` hint when overflow |
| 29 | `chat_view.hpp:422-480` | `multi_choice` shows `[x]/[ ]` but no count of selected/min/max | User doesn't know `minSelect`/`maxSelect` constraints until error | Show `Selected: N/min-max` in dim line above options |
| 30 | `chat_view.hpp:422-480` | `ranker` shows no instruction on how to reorder | "Enter option numbers in order" only in hint line; unclear | Show `1. optA  2. optB` with current ranking numbers |
| 31 | `chat_view.hpp:422-480` | `secret` type shows `*****` but **cursor still visible as `█`** | Cursor reveals input length | Hide cursor for `secret` type |
| 32 | `chat_view.hpp:422-480` | Dialog centered with `panel_2()` bg, cyan border | **No visual distinction** between card types (confirm vs choice vs text); all same chrome | Color border by type: confirm=amber, choice=cyan, text=green, error=red |

---

## 7. Notice / Error / RAW Block Rendering (Medium Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 33 | `chat_blocks.hpp:80-110` `classifyChatBlock` | Lines not starting with space + not recognized header → `Notice` kind | **Notice blocks get dim fg on dark bg** (see #17); also no icon/glyph to distinguish from other blocks | Add `⚠` or `ℹ` prefix for Notice; use `theme::text().fg` not `dim` |
| 34 | `chat_blocks.hpp:80-110` | `RAW` block kind for streaming raw tokens | `RAW` label in transcript but **no visual differentiation** from Thought (both dim) | `RAW` = monospace-ish style? Or at least `theme::amber().fg` on its bg |
| 35 | `inkcell_app_model.hpp:480-520` `rebuildViews` | `TimelineKind::Stream` rows filtered by `showRaw` flag | When `showRaw=false`, stream rows **disappear entirely** — no placeholder indicating hidden raw stream | Show single line `RAW  <N> bytes hidden · r to show` when `!showRaw && streamBytes>0` |
| 36 | `inkcell_app_model.hpp:480-520` | Error rows: `✗ ERROR` label, red fg | **Error body text not wrapped**; long error messages overflow | Apply same wrap logic as other blocks |

---

## 8. Selection / Highlight Affordance in History Focus (Medium Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 37 | `chat_view.hpp:340-380` `drawTranscript` | Selected block: `blockStyle(kind, header, true)` → bg +10/12 RGB | **Selection highlight barely visible** on graphite (bg 25→35, fg bright); neon better but still subtle | Increase selection boost to +20/+25; or invert fg/bg for selected header line |
| 38 | `inkcell_app_model.hpp:350-370` `selectedBlock` | `j/k` move selection; `Enter` drills; `selectedBlock` index into `blockRowIndex` | **No visual selected row number / total** (e.g. `3/47`) | Show `block N/M` in status line or header when `timelineFocus` |
| 39 | `agent_scene.hpp:90-110` | `ArrowUp/Down` scroll line-by-line; `j/k` select blocks | **Confusing dual scroll**: arrows scroll viewport, j/k move selection — but selection not always visible | When `j/k` moves selection off-screen, auto-scroll viewport to keep selected block centered (already in `ensureSelectionVisible` but only called on rebuild) — call it in `selectDelta` |

---

## 9. Color / Contrast: Dim Text, Selected Block (High Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 40 | `cortex_theme.hpp:35-45` `dim()` | Graphite: `rgb(125,125,125)` on `rgb(21,21,21)` = **3.1:1 contrast** (fail AA); Neon: `rgb(116,128,152)` on `rgb(8,11,18)` = **8.4:1** (pass) | **Graphite dim text unreadable** on panel_bg | Raise graphite dim to `rgb(160,160,160)` (5.2:1) or use `text()` fg for all non-muted content |
| 41 | `chat_blocks.hpp:112-145` `blockStyle` | `Thought`/`Raw`/`Notice` → `dim=true` + `fg=dim().fg` | Triple penalty: dark bg + dim fg + dim attr = invisible | Remove `dim` attr for these kinds; use `theme::text().fg` on their respective backgrounds |
| 42 | `chat_blocks.hpp:112-145` | `selected` boosts bg by +10/+12 RGB | On graphite, `User` bg `rgb(25,32,28)` → `rgb(35,42,38)` — barely perceptible | Boost +25/+30; or use `theme::panel_3()` bg for selected (already distinct) |

---

## 10. Long Output Handling (High Impact)

| # | File + Lines | Current Behavior | Problem | Fix |
|---|---|---|---|---|
| 43 | `chat_view.hpp:140-180` `wrapTranscriptRange` | Tool results wrapped same as prose — hard wrap at width | **Massive tool outputs (JSON, logs, file contents) become unreadable wall of text** | Detect "large" blocks (>50 lines or >5KB): render as collapsible with `▼ RESULT <tool> #<id>  <lines> lines · Enter expand` |
| 44 | `inkcell_app_model.hpp:480-520` `rebuildViews` | Every body line → `transcriptView.lines` with `    ` indent | **No virtualization** — 10k line result = 10k entries in `lines` vector, O(n) scroll | Keep virtualization in `ScrollViewState` (offset/limit); only materialize visible lines + 1 screen buffer |
| 45 | `chat_view.hpp:340-380` `drawTranscript` | Scrollbar thumb size = `body.h * body.h / total` | With 10k lines, thumb = 1 pixel; **no thumb drag, no page jump** | Add `Shift+PgUp/PgDn` = jump 10 pages; show line numbers in scrollbar tooltip area |

---

## 11. Additional Polish Items (Low-Medium Impact)

| # | File + Lines | Issue | Fix |
|---|---|---|---|
| 46 | `agent_scene.hpp:90-110` | `g` = refresh nested view — undocumented, no hint | Add to help overlay; show in nested header |
| 47 | `chat_commands.hpp:60-80` `/cp-all` `/cp-raw` | Fallback to `/tmp/mk3-cp-*.txt` — no user feedback on success/fail beyond notice | Show toast-style notice at bottom for 2s: `Copied to clipboard` or `Wrote /tmp/...` |
| 48 | `agent_scene.hpp:145-160` | `vm.hint` strings hardcoded per state | **No dynamic hint for nested view** (shows root hint) | When `!atRoot()`: `↑↓ scroll · j/k select · Enter open · Esc back · g refresh` |
| 49 | `inkcell_app_model.hpp:780-800` | `promptHistory` stores every submit; no dedup beyond last | History grows unbounded; duplicate adjacent entries | Cap at 500 entries; skip if `text == history.back()` |
| 50 | `chat_view.hpp:382-420` `drawHelpOverlay` | `?` closes help but `Esc` also closes — **no hint for `?` toggle** | Add `? toggle help` to footer |

---

## Prioritization (Impact × Effort)

| Priority | Items | Rationale |
|---|---|---|
| **P0 — Do First** | 1, 3, 4, 5, 8, 13, 15, 17, 19, 20, 21, 22, 37, 40, 41, 42 | Visible every session; makes chat feel "broken" or "amateur" |
| **P1 — This Week** | 2, 6, 9, 10, 11, 12, 14, 16, 18, 23, 24, 25, 28, 33, 34, 35, 36, 38, 39, 43, 48 | Daily friction; discoverability; subagent UX is core differentiator |
| **P2 — Next Sprint** | 7, 26, 27, 29, 30, 31, 32, 44, 45, 46, 47, 49, 50 | Polish; power-user features; virtualization is tech debt |

---

## Files to Touch (Minimal Set)

| File | Items |
|---|---|
| `src/ui/chat/chat_view.hpp` | 1, 2, 3, 4, 5, 6, 13, 14, 15, 16, 17, 18, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 43 |
| `src/ui/scenes/agent_scene.hpp` | 7, 19, 20, 21, 22, 23, 24, 38, 39, 46, 48 |
| `src/ui/model/inkcell_app_model.hpp` | 8, 9, 10, 11, 12, 44, 49 |
| `src/ui/chat/chat_blocks.hpp` | 17, 33, 34, 41, 42 |
| `src/ui/theme/cortex_theme.hpp` | 40 |
| `src/ui/chat/chat_commands.hpp` | 47 |
| `src/ui/chat/ask_dialog_model.hpp` | (validation messages for 29, 30) |

---

## Validation Checklist (Run After Fixes)

- [ ] Graphite theme: all dim text ≥ 4.5:1 contrast on panel_bg
- [ ] Neon theme: no regressions
- [ ] Empty chat shows centered hint
- [ ] Header shows agent name + provider/model + session
- [ ] Status line shows live elapsed when running; last turn summary when idle
- [ ] Composer: `Ctrl+U/K/W` work; `Enter` = newline, `Ctrl+Enter` = send
- [ ] Paste >5 lines → inline confirm
- [ ] Code fences: horizontal scroll or soft-wrap with continuation glyph
- [ ] Subagent drill: breadcrumb in header; metrics reflect current agent; `← Back` hint
- [ ] Help overlay: grouped, context-aware, shows current theme
- [ ] Ask dialog: scrollable; secret hides cursor; type-colored borders
- [ ] Selection highlight clearly visible in both themes
- [ ] Large tool result (>50 lines) → collapsible summary
- [ ] Scrollbar thumb usable at 10k lines

---

*Generated by audit sweep — no code modified. All fixes are localized, single-file or two-file changes.*