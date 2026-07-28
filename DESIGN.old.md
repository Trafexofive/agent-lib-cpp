# sbtui Design Specification

**Version 3 — Complete**  
**Status:** Authoritative. This document supersedes all prior versions in full. Previous content from v2 has been absorbed and expanded; it is not carried forward alongside this text.

**Scope**  
This specification defines *what* any sbtui application must be — its observable behavior, information architecture, visual language, state handling, and interaction model. It does not prescribe implementation details, frameworks, languages, or rendering libraries.

It covers two classes of application:
- **API-client TUIs**: network-backed, subject to latency, failure, authentication, and rate limits.
- **Local / client-side TUIs**: fully offline by definition, subject to large datasets, persistence, and startup costs.

Sections explicitly tagged **[API]** or **[LOCAL]** apply only to that class. Untagged sections apply to both.

---

## 0. Thesis: The Breathing TUI

A static TUI renders correctly once and then remains visually unchanged until the next external input. A *breathing* TUI remains legible and truthful at every point in its lifecycle: cold start, loading, populated, partial, stale, erroring, empty, recovering, and idle.

Breathing does not mean animation or motion. It means the visible interface always corresponds exactly to the current model state, including the states most TUIs treat as afterthoughts or error paths. The operator can answer “what is happening right now?” from a single glance without guessing whether the interface is frozen, working, or waiting.

**Corollary**: An interface that only looks correct in its happy-path populated state has not been designed; it has been demoed. Every subsequent section of this spec treats non-happy-path states as first-class design targets.

---

## 1. Invariants

These ten rules are absolute. They are never violated by any screen, mode, data source, or app that claims sbtui compliance.

1. **Edge Safety** — No content may touch the physical edges of the terminal. Minimum outer inset is 2 cells on every side.

2. **Panel Padding** — Every panel provides at least one column of horizontal padding on each side.

3. **Multi-Cue Selection** — Selected or focused state must be communicated by at least two simultaneous, independent visual cues. Color alone is never sufficient.

4. **No Idle Motion** — Motion or visual change is permitted only when it reflects a genuine, currently-true state transition. Decorative, perpetual, or idle animation is prohibited.

5. **Deterministic Snapshot Rendering** — When rendered in snapshot mode with identical input state, output must be byte-identical across runs, environments, and terminal emulators.

6. **Preview Before Mutation** — Any action that mutates filesystem, network, or persistent local state must first render a clear, non-committal preview of its exact effect.

7. **Complete State Coverage** — Every screen the operator can reach must define explicit renderings for its loading, empty, and error conditions. “It just doesn’t render anything” is never acceptable.

8. **Discoverable Keybindings** — Every keybinding that exists must be discoverable from within the running application (footer hint or help overlay). No secret or undocumented keys.

9. **Distinct Confirmation for Destruction** — Destructive actions (irreversible or high-cost) require a separate, explicit confirmation step. Safe actions execute immediately; they never require confirmation theater.

10. **Non-Blocking Cancelable Operations** — The operator must never be blocked on a spinner for an operation that is cancelable at the protocol or filesystem level without a visible means to cancel or background it.

A view that cannot satisfy every invariant requires redesign of the view. Exceptions are not carved into the spec.

---

## 2. Information Architecture (IA)

Before any layout, color, or keybinding decision, the object model exposed to the operator must be defined.

**Required deliverables at IA stage (still design-only):**
- An entity-relationship-action table
- A view-transition diagram showing every reachable view and the inputs that produce transitions between them

**Entities**  
The nouns the application operates on (templates, tasks, connections, messages, etc.). Each entity must have exactly one canonical list/grid representation and one canonical detail representation. Additional representations require explicit justification.

**Relationships**  
How entities reference one another (parent/child, tags, dependency graphs, etc.). Relationships must drive navigation affordances (“jump to related”) in addition to any descriptive text in detail views.

**Actions**  
Verbs available for each entity, partitioned at IA time into:
- **Safe** — reversible or low-cost; execute immediately
- **Destructive** — irreversible or high-cost; require distinct confirmation

This partition is made once per action type during IA and is not re-decided per screen.

**Views**  
The finite set of modes the application can occupy (list, detail, edit, search, command palette, modal confirm, help, etc.). Views form an enum. If the complete set cannot be enumerated, the IA is incomplete.

---

## 3. Density Tiers & Responsive Behavior

Terminal real estate varies dramatically. The same application must degrade gracefully from a 200-column desktop session to an 80-column SSH session without data loss or broken layout.

| Tier     | Width       | Behavior |
|----------|-------------|----------|
| **Wide** | ≥ 160 cols  | Full multi-pane layout; all metadata columns visible simultaneously |
| **Standard** | 100–159 cols | Primary layout; secondary metadata collapses into detail panel only |
| **Narrow** | 80–99 cols  | Single pane at a time; navigation becomes stack-based (push/pop) rather than side-by-side |

**Below 80 columns**: The application declares its minimum supported width and renders a centered, non-interactive “Resize your terminal to at least 80 columns” notice. It does not attempt a broken or truncated layout.

Tier transitions occur live on terminal resize events. Reflow must preserve current selection, focus, and scroll position wherever semantically possible. No state is lost or reset on resize.

---

## 4. Visual Language

### 4.1 Hierarchy Over Decoration

- Background panels are the primary containment mechanism; borders are secondary and rare.
- Padding is mandatory (≥ 1 column) and never omitted for visual effect.
- Box-drawing characters (╭╮╰╯) are reserved for modal cards and dialogs only. They are never used for repeated per-row or per-section containers.
- ASCII rules and tree glyphs are preferred over Unicode box-drawing when they produce cleaner visual rhythm.

**Anti-pattern — equal-weight box spam** (avoid):
```
╭─ title ─────╮  ╭─ title ─────╮  ╭─ title ─────╮
│             │  │             │  │             │
╰─────────────╯  ╰─────────────╯  ╰─────────────╯
```
Equal visual weight prevents the eye from quickly locating what matters.

**Preferred pattern — weighted panels + ASCII section rules**:
```
APP NAME
one-line value proposition
[status] [mode]                              [conn: live ●]

 nav                    list / grid content              detail / action
 route a                item title                       selected item
 route b                item summary                     metadata
 route c                #tags                            command preview
```

### 4.2 Typography (Three Levers Only)

Terminal cells provide exactly three reliable emphasis channels:
- **Primary** (titles, active selection): brightest foreground + surrounding whitespace
- **Secondary** (body copy): standard foreground, no special treatment
- **Tertiary** (metadata, timestamps, hints): `dim()` — visually lighter than body copy

Bold and underline escape codes must never be the sole differentiator; they render inconsistently across emulators. Color + spatial isolation is the reliable channel.

### 4.3 Glyph Vocabulary (Constrained & Reusable)

A single, application-wide glyph set substitutes for iconography. It is defined once and reused consistently.

| Glyph | Meaning |
|-------|---------|
| `●`   | live / connected / active |
| `○`   | idle / disconnected / inactive |
| `◐`   | partial / degraded / syncing |
| `✓`   | confirmed / success |
| `✗`   | failed / error |
| `!`   | warning / attention needed |
| `>` or `│` | selection / focus marker |
| `…`   | truncated / more available |

New glyphs require justification and addition to this shared vocabulary rather than per-screen invention.

---

## 5. Layout Grammar

```
page inset(2)
├── hero / topbar
│   ├── title + value prop
│   ├── status chips
│   └── connection / health indicator
├── body (tier-dependent)
│   ├── nav / sidebar          (routes + counts)
│   ├── primary content        (list / grid / detail + all §9 states)
│   └── action / detail stack  (metadata + primary action + preview)
├── overlays
│   ├── toast stack            (transient, non-blocking, ≤3 visible)
│   ├── modal sheets           (blocking, dim underlay)
│   └── command palette        (blocking, dim underlay)
└── footer
    ├── left:  mode-aware key hints
    └── right: global status (connection, sync, pending ops)
```

**Strict rules**:
- Major columns are separated by empty gutters only. Vertical rule characters are never used as separators.
- List and card items always carry structured fields (title + metadata + tags). Flat single-line lists are prohibited.
- Section labels use full-width ASCII rules: `--- label ------------------------------------------------`
- The footer always shows both contextual key hints *and* global status. It never shows only one.
- Modals dim the background and own all input while open.
- Toasts never block input and never stack more than three deep; additional toasts collapse into a “+N more” indicator.
- Every page is an explicit, enumerable view (derived from §2). There are no hidden conditional fragments.

---

## 6. Color / Style Token System

### 6.1 Token Maturity Model

Tokens progress through three strict levels. A token may not skip a level.

1. **Local** — Defined and used inside a single application’s theme asset only.
2. **Shared candidate** — A second application requires the identical semantic. The token is duplicated verbatim (same name, same value) rather than each application inventing its own name.
3. **Registry** — After two or more applications have used the token identically for at least one release cycle with no divergence, it is promoted into the shared sbtui theme registry. Applications then import rather than duplicate.

Premature abstraction on a single data point is prohibited. Silent divergence between two applications claiming “the same” token is a signal to reconcile, not to ignore.

### 6.2 Semantic Token Set

| Token            | Purpose |
|------------------|---------|
| `base_bg()`      | Terminal page background |
| `panel_bg()`     | Normal panel background |
| `panel_2()`      | Card / list-row background |
| `panel_3()`      | Selected / elevated row background |
| `dim()`          | Metadata, hints, inactive text, tertiary emphasis |
| `text()`         | Body copy, secondary emphasis |
| `bright()`       | Primary emphasis (titles, active selection foreground) |
| `cyan()`         | App title / section emphasis |
| `green()`        | Selected / success / safe-confirmed target |
| `amber()`        | Command / warning / pending action / degraded state |
| `red()`          | Destructive / error / disconnected |
| `selected_style()` | Active row text rendered on elevated background |
| `hot_bg()`       | App-specific highlight (exact mutation target, active search match) |

Exact color values (ANSI, 256, truecolor) are defined by the active theme. Only the semantic contract is standardized.

### 6.3 No-Color Degradation

Every semantic distinction carried by color **must** have a non-color fallback (glyph from §4.3 or explicit text label). This requirement exists because:
- Some terminals and operators run without 256-color support
- Screenshots and terminal recordings frequently lose color fidelity
- Some operators are colorblind

Design the glyph or label first. Color is an accelerant layered on top, never the sole carrier of meaning.

---

## 7. Border & Containment Policy (Decision Tree)

Apply the following decision tree in order. The default answer is “no border.”

1. Does removing the border lose information that a background fill cannot carry (e.g., containment of a floating modal or command palette)? → Border permitted.
2. Is this element a per-row list or card item? → No border, ever. Use `panel_2()` / `panel_3()` + selection marker instead.
3. Is this element a section boundary? → ASCII rule (`---`), never a box.
4. Is this element a filesystem or hierarchy tree? → ASCII tree glyphs (`|--`, ``--`), never boxes.

Borders are added only when a specific, documented reason exists. They are not the default.

---

## 8. Selection & Focus Policy

### 8.1 Single-Pane Selection
Requires **all three** cues simultaneously:
- Elevated background (`panel_3()`)
- Foreground color shift (`green()` or `selected_style()`)
- Leading marker (`>` or `│`) — survives no-color terminals and screenshots

### 8.2 Multi-Pane Focus
When the layout contains multiple independently navigable panes (nav / list / detail), exactly one pane holds *focus* at any moment. Focus is distinct from selection inside a pane:

- The focused pane’s current selection receives the full three-cue treatment (§8.1).
- Unfocused panes show their last selection in a muted form (elevated background only, no color shift, no marker). This communicates “you were here” without competing for attention.
- Moving focus between panes uses a distinct keybinding class (e.g., `Tab` / `Shift-Tab`) from moving selection inside the current pane (arrows / `hjkl`). The two classes must never be overloaded on the same keys without an explicit mode indicator.

### 8.3 Modal Focus Trap
While a modal or command palette is open, it is the sole focusable surface. Background panes continue to render their last-focused state (muted per §8.2) but consume no input. Closing the modal restores focus exactly to the pane and selection that were active immediately before the modal opened. Focus is never reset to a default.

---

## 9. State Taxonomy (The Core of Breathing)

Every screen must define a concrete rendering for every applicable state below. “It just doesn’t render anything” is never an acceptable answer.

| State              | Definition | Rendering Requirement |
|--------------------|------------|-----------------------|
| **Loading (cold)** | First request/read in flight; no prior data available | Skeleton matching final layout shape, or minimal centered indicator for sub-200 ms operations. Never a blank screen. |
| **Loading (refresh)** | Re-fetch in progress while prior data is still visible | Prior data remains visible; subtle in-place indicator (dim overlay or corner glyph). Never a full-screen loader that discards context. |
| **Populated**      | Data present and healthy | Happy path. Must still surface item counts, pagination state, or “more available” indicators. |
| **Empty (genuine)** | Zero items; not caused by filter or error | Explicit empty-state message that distinguishes “nothing here yet” from “nothing matched your current filter.” |
| **Empty (filtered)** | Zero items because of active search/filter | Message includes the active filter value and a single-key “clear filter” affordance. |
| **Partial**        | Some data loaded successfully; some items failed or pending | Successful items render normally. Failed/pending items show inline per-item markers. Never a page-level banner that hides the successful items. **[API]** |
| **Stale**          | Data present but known to be out of date (reconnect after disconnect, cache past TTL) | Visible staleness indicator (timestamp or glyph). Non-blocking. **[API]** |
| **Error**          | Request/read failed entirely | Error message includes the actual cause (not generic “something went wrong”) plus a retry action when retry is meaningful. |
| **Offline**        | No connectivity | Distinct from generic error. Explicit offline indicator in footer/status. Queued-action indicator if actions are being buffered for reconnect. **[API]** |
| **Rate-limited**   | Backend is throttling the client | Explicit countdown or backoff indicator. Never presented as a generic error. **[API]** |
| **Permission-denied** | Authenticated but unauthorized for this resource | Message explains exactly what permission is missing. Distinct from generic error. **[API]** |

**Definition of done for any screen**: Every applicable row above has a defined, reviewed rendering before the screen is considered complete.

---

## 10. Mutation & Preview Pattern

Any flow that mutates persistent state (filesystem, network, or local database) must follow this sequence:

1. Clearly identify the source item(s) and current state.
2. Clearly identify the target / destination.
3. Render a structural preview (tree, diff, or equivalent) with the delta highlighted.
4. Classify the action as safe or destructive per the §2 IA partition.
5. If destructive, require a distinct confirmation step (separate keypress or screen). Safe actions execute immediately.

No mutation-triggering keybinding may be reachable until the full preview has rendered. This is a hard gate.

**Preview minimum content** (illustrative):
```
selected: <id>
source:   <current state>
target:   <destination state>
class:    safe | destructive

command preview
<exact operation that will be performed>

result preview
<tree or diff with change highlighted>
```

### 10.1 Undo
Every safe mutation that is locally reversible must surface an undo action via toast immediately after execution (“Moved to X — undo”). The toast expires after a defined window (default: until the next mutating action or 10 s, whichever comes first).

Destructive actions that happen to be technically reversible (e.g., soft-delete) still pass through the distinct confirmation step. Undo is a safety net in addition to confirmation, never a substitute for it.

---

## 11. Motion & Transition System

Motion exists only to help the operator maintain context across genuine state changes. It is a continuity aid, not decoration or engagement.

**Core Principles**:
- Motion is triggered exclusively by real state transitions (see invariant 4).
- Every transition is interruptible: a new navigation or selection event immediately cancels any in-progress transition and starts a fresh one from the current interpolated visual state.
- Batch updates affecting three or more items (e.g., filtered list re-population) must use staggered timing so the eye can distinguish persisted items from changed or new items.
- No motion is applied to connection status changes, idle states, or any situation lacking an underlying data or model change.
- Durations should feel responsive and lightweight. Typical perceptual targets: 80–150 ms for intra-pane selection movement, 120–200 ms for pane or route transitions. Exact timing and easing curves are implementation details provided the perceptual goals and interruptibility requirements are met.

**Explicitly Rejected Patterns**:
- Any animation that continues while the underlying model state is idle.
- Fake live tickers, progress bars, or pipelines with no corresponding operation.
- Pulsing, scanning, or decorative effects on static targets.
- Simultaneous snap updates on lists of three or more items (violates stagger requirement).

**Snapshot vs. Live Mode**:
- Snapshot mode resolves every transition to its end state with zero motion; output is byte-identical run-to-run.
- Live mode applies motion only on the triggers defined above.
- Idle live mode is visually indistinguishable from snapshot mode aside from the terminal’s native caret blink (if present).

---

## 12. Notification & Feedback Channels

Three distinct, non-conflated channels exist:

| Channel       | Persistence | Blocking? | Use Case | Example |
|---------------|-------------|-----------|----------|---------|
| **Toast**     | Transient (auto-dismiss 4 s default; 8 s for errors) | No | Confirmation of completed safe actions, undo offers, background operation completion | “Item moved — undo” |
| **Inline Banner** | Persistent until resolved | No (but attached to affected pane) | Partial, stale, or scoped error states | “3 items failed to sync” above the relevant list |
| **Status Bar (footer, right)** | Always visible | No | Ambient global truth: connection health, pending op count, sync status | “conn: live ●  |  2 pending” |

**Rule**: Information that demands the operator’s attention regardless of current activity belongs in a banner or status bar. Toasts are reserved for information the operator can safely miss.

---

## 13. Navigation & Wayfinding

- **Mode Indicator** — The current view/mode is always explicitly legible from the topbar or footer. The operator never infers mode from context alone.
- **Breadcrumb** — Any view reached by drill-down (list → detail → sub-detail) displays the full path back, not merely a generic “back” affordance.
- **Command Palette** — A single, consistent keybinding (convention: `:` or `Ctrl+K`) opens a fuzzy-searchable list of every available action and route in the application. Required for any application with more than approximately eight distinct actions.
- **Jump-to-Related** — Entities that participate in relationships (§2) expose a direct keybinding to navigate to a related entity’s detail view.

---

## 14. Search & Filter

- Search is fuzzy (substring / subsequence) by default. Exact-match is available but is never the only mode.
- Active filters and search state are always visible as chips or labels adjacent to the list. They are never hidden inside menus.
- A single keybinding clears all active filters and search state. The affordance is visible whenever any filter or search is active.
- **[LOCAL]** For large local datasets, index or search readiness status is surfaced (e.g., “indexing… 40 %”). Incomplete results are never returned silently during index construction.
- **[API]** Server-side search versus client-side re-filtering of already-loaded data must be visually distinguished (glyph or label) so the operator knows whether the entire dataset or only the current page is being searched.

---

## 15. Bulk Actions & Multi-Select

- Multi-select is an explicit mode entered by a dedicated keybinding and indicated in the footer/mode indicator. It is never an accidental side-effect of holding a modifier.
- The count of selected items is always visible while multi-select mode is active.
- Bulk actions follow the same safe/destructive partition and confirmation rules as single-item actions (§10). Bulk destructive confirmations must state the count (“Delete 14 items?”) rather than reusing single-item copy.
- Exiting multi-select mode without performing an action is always possible with a single keypress (Escape convention). The operator is never forced to resolve the selection first.

---

## 16. Onboarding & First-Run

- The very first launch with no prior state renders a distinct first-run view (not the standard empty state). It explains the immediate next action(s): connect, import, create, authenticate, etc.
- **[LOCAL]** First-run assumes no local data yet. **[API]** First-run assumes no connection or auth configured yet.
- The first-run view appears at most once per fresh install or configuration. Subsequent empty states use the standard §9 empty-state rendering.
- Any multi-step setup sequence (auth, initial config, import) shows clear progress (“Step 2 of 4”) so the operator always knows how many steps remain.

---

## 17. Input & Keybinding Philosophy

- **Global keybindings** (quit, help, command palette, etc.) are identical across every sbtui application. An operator who learns one application’s globals knows them for all.
- **View-local keybindings** are always shown in the footer for the current mode. They are never documented only in a separate help screen.
- A full help overlay (`?` convention) lists every keybinding available in the current view plus all globals. Groupable by category when the list exceeds ~10 entries.
- No keybinding is hidden or power-user-only. If a keybinding is implemented, its discoverability (footer or help) is implemented in the same change.
- Modal input states (typing into a search field) are visually distinct from modeless navigation (distinct input-mode indicator) so the operator always knows whether keystrokes are being captured as text or as commands.

---

## 18. API-Client Specific Concerns [API]

- Connection and authentication status are permanently visible in the topbar or footer. The operator never discovers a problem only when an action fails.
- Retry and backoff behavior on failed requests is visualized (countdown, attempt count). A stalled screen must communicate whether it is still retrying or has given up.
- **Optimistic UI** is permitted only for actions classified *safe* in the §2 IA partition. The result is rendered immediately with a pending indicator and later reconciled (confirmed or rolled back with visible error). Destructive actions always wait for server confirmation before any visual mutation.
- **Pagination / streaming**: Current position and total (when known) are always visible (“23 of ~140”, “23 more available”). Streamed/infinite data shows a distinct “loading more” indicator at the scroll boundary, visually different from initial-load state.
- On reconnect after disconnect, any queued actions are shown resolving. They are never silently flushed.

---

## 19. Local / Client-Side Specific Concerns [LOCAL]

- The persistence boundary is explicit to the operator when it matters (“unsaved changes” indicator when a save step exists; no indicator when state is auto-persisted).
- Lists larger than a few hundred items use virtualization. Scroll position and selection survive virtualization with no visible jump or flicker at the window boundary.
- Index or search build status is surfaced per §14. Incomplete results are never returned silently.
- Cold-start data loading that exceeds ~300 ms receives the Loading (cold) treatment from §9 rather than a frozen splash screen with no indicator.

---

## 20. Component Contract Library

A contract defines required inputs and required visual states only. It contains no implementation guidance.

Promotion into the shared sbtui component set occurs only after a second independent application consumes the component under the identical contract with no modifications.

| Component            | Required Inputs                                      | Required Visual States |
|----------------------|------------------------------------------------------|------------------------|
| TopBar               | title, subtitle, chips[], connection/health indicator | static, degraded |
| SidebarNav           | routes[] (label, count), focused route               | idle, focused |
| ListLike (Card/Row)  | items[] (title, summary, id, tags[]), selected index, per-item status | idle row, selected row (§8.1), muted-selected (§8.2), per-item pending/error marker (§9 partial) |
| DetailPanel          | key/value pairs, semantic style per row              | static, loading (skeleton) |
| CommandPreview       | command/operation string, safe/destructive class     | static, copyable |
| StructurePreview     | tree/diff, highlighted delta                         | static, highlight-on-node |
| PrimaryActionCard    | action label, preview, safe/destructive class        | idle, pending, confirm-required |
| KeyFooter            | mode-aware key hints[], global status                | static |
| Toast                | message, kind (info/success/error), optional undo    | entering, visible, dismissing |
| Banner               | message, kind, optional retry action                 | visible, dismissing |
| EmptyState           | kind (genuine/filtered/first-run), message, optional action | static |
| LoadingSkeleton      | shape matching target layout                         | static (no shimmer or animation) |
| CommandPalette       | fuzzy-searchable actions[]/routes[]                  | closed, open, filtering, no-match |
| MultiSelectBar       | selected count, available bulk actions[]             | active, confirm-pending |
| ConfirmModal         | action description, item count if bulk               | visible, focus-trapped (§8.3) |

---

## 21. Accessibility & Degradation

- Every color-carried distinction has a glyph or explicit text fallback (§6.3).
- Minimum supported width is declared per application (§3). Below that width the application renders a resize notice rather than a broken layout.
- No functionality is mouse-only. Every action is reachable by keyboard; mouse support, when present, is strictly additive.
- Timing-sensitive interactions (double-press, chord sequences) always have a single-key alternative. No function requires fast reflexes.

---

## 22. Asset Architecture (Logical Separation)

Regardless of implementation language or framework, the following concerns must remain strictly separated:

- Data contracts, page/view state machines, and derived navigation paths — **model**
- Color and style tokens with zero state dependency — **theme**
- Generic drawing, measurement, and layout helpers — **layout**
- Page, view, and modal rendering logic (no key handling) — **views**
- Scene/controller: keybinding dispatch, transition orchestration, mode state, snapshot vs. live entry points — **app**

**Dependency rule**: One-way only.  
`entrypoint → controller → views → {layout, model, theme}`

Any dependency arrow pointing backward is a review-blocking defect.

---

## 23. Explicit Non-Goals

The following patterns are deliberately out of scope and will be rejected in review:

- Idle or decorative animation of any kind
- Per-row borders regardless of available terminal width
- GUI/TUI hybrids whose primary affordances require a mouse
- Introduction of a new theme token into the shared registry before a second consuming application exists at matching maturity (§6.1)
- Any mutation path that bypasses the structural preview gate (§10)
- Any destructive action lacking a distinct confirmation step, regardless of how “obviously intentional” the triggering gesture appears
- Any screen shipped without defined loading, empty, and error renderings (§9)
- Any hidden or undocumented keybinding (§17)

---

## 24. Definition of Done (Spec-Level Checklist per Application)

- [ ] Information-architecture table (entities / relationships / actions / views) exists and every view is reachable via the documented transition diagram (§2)
- [ ] Density tiers and breakpoints are defined (defaults from §3 or explicit deviations) (§3)
- [ ] Every applicable state from the §9 taxonomy has a concrete, reviewed rendering on every screen
- [ ] Every motion trigger that will produce visual change has an entry in §11 with clear perceptual goals
- [ ] Every promotable component has a contract recorded in §20
- [ ] Border policy decisions are fully resolved by the §7 decision tree with no residual “use judgment” cases
- [ ] Every keybinding appears in either a footer hint or the help overlay for its view (§17)
- [ ] The non-goals list (§23) covers every pattern previously rejected during review
- [ ] **[API]** Connection, auth, retry, offline, rate-limit, and permission states are all rendered per §9 and §18
- [ ] **[LOCAL]** Persistence boundary and large-dataset virtualization behavior are defined per §19

---

## A. App-Specific Fill-In Template

When applying this spec to a concrete application, complete the following section. Empty is the expected and preferred state for most fields; deviations are documented explicitly in the next section.

**App name**:  
**Class**: API-client | client-side | hybrid  
**One-line value proposition**:  

**IA deliverables (§2)**:  
Entities / relationships / actions / views table:  
View-transition diagram:  

**Density tier breakpoints** (if different from §3 defaults):  

**App-local tokens** beyond the §6.2 set (name + purpose only):  

**App-specific mutation flows** (map each to the §10 preview pattern):  

**State-taxonomy notes** (§9): Any state requiring custom handling beyond the standard table:  

---

## Deviations from This Specification

List every intentional deviation from §0–§23, the section(s) affected, and the concrete rationale.  

An empty deviations section is the expected state for a compliant application. Any non-empty entry must survive review.

---

**End of sbtui Design Specification v3**
