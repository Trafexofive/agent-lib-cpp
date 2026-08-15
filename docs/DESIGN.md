# gtui — Design Specification

**Applies:** sbtui Design Specification v3
**App class:** API-client
**Status:** Draft for review — flag anything below with the review notes it references.

---

## 0. Resolutions carried in from spec review

Before filling the template, four ambiguities in the base spec had to be closed one way or another to write anything concrete. Documented here so they're attackable, not buried:

1. **Preview gate vs. "safe executes immediately" (Invariant 6/9, §10).** Resolved per-action-class: safe actions render their preview *synchronously in the same interaction* (the compose box you type into *is* the preview; hitting submit executes with no separate gate). Destructive actions render preview as a distinct blocking modal requiring a separate confirm keypress. See §4 below.
2. **Snapshot determinism (Invariant 5) vs. live clock/capability reads.** Resolved by injection contract — see §7.
3. **"Applicable" state coverage (§9/§24).** Resolved by a structural rule, not per-screen judgment — see §6.
4. **Toast vs. Banner for errors (§12).** gtui narrows the base spec's toast contract: toasts never carry error content, full stop. See §8.

---

## A. App-Specific Fill-In

**App name:** gtui
**Class:** API-client
**One-line value proposition:** Terminal-native operator console for a self-hosted Gitea instance — repos, issues, PRs, and notifications without leaving the keyboard.

---

## 1. Information Architecture

### 1.1 Entities

| Entity | Canonical list | Canonical detail | Notes |
|---|---|---|---|
| Repo | RepoList | RepoDetail (tabs: issues / PRs / branches) | |
| Issue | IssueList (scoped to repo, or aggregate "My Issues") | IssueDetail | |
| PullRequest | PRList (scoped to repo) | PRDetail | |
| Notification | NotificationList | *(none — see below)* | |
| Branch | *(none — see below)* | *(none — see below)* | |
| Comment | *(none — see below)* | *(none — see below)* | |

**Justified exceptions to "one list + one detail per entity":**
- **Notification** has no detail view. Selecting one jumps directly to the target Issue/PR detail via §2's jump-to-related mechanism. A separate NotificationDetail would duplicate IssueDetail/PRDetail for no benefit at single-operator scale.
- **Branch** has no standalone list or detail. It surfaces as a tab inside RepoDetail and as base/head fields inside PRDetail. No cross-repo branch browsing need exists for this tool.
- **Comment** has no standalone list or detail. It's rendered inline inside IssueDetail/PRDetail only. No cross-repo comment search is in scope.

### 1.2 Relationships

| From | To | Cardinality | Navigation affordance |
|---|---|---|---|
| Repo | Issue | 1:N | RepoDetail → issues tab |
| Repo | PullRequest | 1:N | RepoDetail → PRs tab |
| Repo | Branch | 1:N | RepoDetail → branches tab |
| PullRequest | Issue | N:N (closes/references) | `g i` from PRDetail jumps to linked issue |
| PullRequest | Branch | 2 (base, head) | inline in PRDetail, `g b` jumps to branches tab |
| Issue / PR | Comment | 1:N | inline, no separate jump needed |
| Notification | Issue \| PullRequest | 1:1 (target) | `enter` on notification jumps to target detail |

### 1.3 Actions (safe / destructive partition)

| Action | Entity | Class | Reversibility |
|---|---|---|---|
| Comment | Issue, PR | safe | edit/delete after the fact |
| Add/remove label | Issue, PR | safe | toggle again |
| Assign/unassign | Issue, PR | safe | toggle again |
| Star | Repo | safe | toggle again |
| Mark read/unread | Notification | safe | toggle again |
| Mark all read | Notification (bulk) | safe | per-item unread is not individually recoverable, but low-cost and non-destructive to actual state |
| Request review | PR | safe | withdrawable |
| Approve review | PR | safe | withdrawable |
| Reopen | Issue, PR | safe | can be closed again |
| **Close** | Issue, PR | **destructive** | technically reopenable, but changes triage state and can trigger downstream automation (webhooks) — treated as high-cost |
| **Merge** | PR | **destructive** | irreversible on the git graph; triggers CI/deploy side effects |
| **Delete branch** | Branch | **destructive** | irreversible; loses ref unless recovered from reflog on the server, which the operator has no TUI-level visibility into |

Repo creation, repo deletion, org/team management: **out of scope** — see §9 Deviations.

### 1.4 Views (finite enum)

```
RepoList · IssueList (repo-scoped + "My Issues" aggregate) · IssueDetail
PRList · PRDetail · NotificationList
CommandPalette (modal) · ConfirmModal (modal) · HelpOverlay (modal)
AuthSetup (first-run) · ResizeNotice (sub-80-col guard)
```

11 views. Command palette is mandatory per §13 (action count exceeds 8).

### 1.5 View-transition diagram

```
                        ┌───────────────┐
       first run,       │  AuthSetup    │
       no token   ─────▶│ (2 steps:     │
                         │  url, token)  │
                         └───────┬───────┘
                                 │ token verified
                                 ▼
                         ┌───────────────┐
                  ┌─────▶│   RepoList    │◀────────────────┐
                  │      └───┬───────┬───┘                  │
            Esc / q│    enter│       │n                     │Esc (back)
                  │   (repo) │       │(notifications)        │
                  │          ▼       ▼                       │
                  │  ┌───────────────┐   ┌───────────────────┴─┐
                  │  │  RepoDetail   │   │  NotificationList    │
                  │  │ issues/PRs/   │   └──────────┬────────────┘
                  │  │ branches tabs │              │ enter (jump-to-related)
                  │  └──┬────────┬───┘              │
                  │ enter│        │enter             │
                  │(issue)       (PR)                │
                  │     ▼        ▼                  │
                  │ ┌──────────┐ ┌─────────┐         │
                  └─┤IssueDetail│ │PRDetail │◀────────┘
                    └──────────┘ └─────────┘

  overlays (reachable from any non-modal view, focus-trapped per §8.3):
    :  / Ctrl+K   → CommandPalette
    ?             → HelpOverlay
    x / m / d     → ConfirmModal   (close / merge / delete-branch — destructive only)
    width < 80    → ResizeNotice   (replaces body entirely, not a stack entry)
```

---

## 2. Density Tiers

No deviation from spec defaults (Wide ≥160, Standard 100–159, Narrow 80–99, sub-80 resize notice).

**Wide** — three-pane, `repos | issue/PR list | detail`, all simultaneously visible:

```
gtui                                                  [conn: live ●] [auth: ok]
self-hosted gitea operator console

 repos (12)            issues · gtui/infra (23 open)          #482
 > gtui/infra   ●       > #482  fix: wg handshake retry  >    fix: wg handshake retry
   gtui/mail    ○         #481  postfix queue purge cmd       opened 2d ago · cleverlord
   gtui/dns     ◐         #479  rspamd score tuning           labels: bug, infra
                          #470  nginx upstream flap           assignees: cleverlord
                                                               3 comments · 1 linked PR

 [tab] switch pane  [j/k] move  [enter] open  [c] comment  [x] close (destructive)
 conn: live ●  |  0 pending  |  last sync 4s ago
```

**Standard** — nav collapses into the palette; two-pane `list | detail`:

```
gtui › gtui/infra › issues                             [conn: live ●] [auth: ok]

 issues (23 open)                              #482 · fix: wg handshake retry
 > #482  fix: wg handshake retry >              opened 2d ago · cleverlord
   #481  postfix queue purge cmd                labels: bug, infra
   #479  rspamd score tuning                     assignees: cleverlord
   #470  nginx upstream flap                     3 comments · 1 linked PR (#483)

                                                 --- comments ------------------
                                                 cleverlord (2d ago)
                                                 wg0 handshake drops under...

 [:] palette  [/] search  [c] comment  [x] close  [tab] focus detail
```

**Narrow** — single pane, push/pop stack:

```
gtui/infra › issues                                            [conn: live ●]

 > #482  fix: wg handshake retry
   #481  postfix queue purge cmd
   #479  rspamd score tuning
   #470  nginx upstream flap

 [enter] open  [/] search  [esc] back  [:] palette
```

Resize preserves selection/scroll per §3's requirement — no reset on tier transition.

---

## 3. App-local tokens

| Token | Purpose | Maturity (§6.1) |
|---|---|---|
| `merged_purple()` | PR state = merged. Neither `green()` (open/success) nor `red()` (closed/error) fits — Gitea's own convention uses a distinct hue for merged, and collapsing it into green loses the open-vs-merged distinction at a glance. | Local |

No other new tokens. `amber()` already covers CI-pending/degraded, `red()` covers closed/error — reusing these rather than inventing per-screen variants, per §6.1's premature-abstraction prohibition.

---

## 4. Mutation flows (§10, with safe/destructive split resolved per §0.1)

### 4.1 Safe: Comment on Issue/PR
Preview = the compose box itself (WYSIWYG markdown render as you type). No separate gate. `Ctrl+Enter` executes immediately — optimistic UI posts with a pending glyph, reconciled on server ack. Toast on success: `"Comment posted — undo (delete)"`, 4s window.

### 4.2 Safe: Label add/remove
Toggle in a label picker overlay. Each toggle sends the PATCH immediately, optimistic UI, no confirmation step. Undo = toggle again.

### 4.3 Destructive: Close issue
```
selected: #482
source:   open, 3 comments, assignee: cleverlord
target:   closed
class:    destructive

command preview
PATCH /repos/cleverlord/gtui-infra/issues/482  {"state": "closed"}

[y] confirm   [esc] cancel
```
Waits for server ack before any visual mutation — no optimistic UI on destructive actions (§18).

### 4.4 Destructive: Merge PR
```
╭─ merge pull request ───────────────────────────────────────────╮
│                                                                 │
│  #483  fix: wg handshake retry → main                          │
│                                                                 │
│  strategy:  ( ) merge commit  (•) squash  ( ) rebase           │
│  ci:        ✓ 4/4 checks passed                                │
│  reviews:   ✓ approved (1/1 required)                          │
│                                                                 │
│  command preview                                               │
│  POST /repos/cleverlord/gtui-infra/pulls/483/merge              │
│    {"Do": "squash", "delete_branch_after_merge": true}          │
│                                                                 │
│  class: destructive                                             │
│                                                                 │
│  type the PR number to confirm: [___]                          │
│  [esc] cancel                                                   │
╰──────────────────────────────────────────────────────────────╯
```
Confirmation exceeds the spec's minimum bar (typed number, not a single keypress) — irreversible on the git graph, can trigger CI/deploy automation. Logged as a deviation in §9.

### 4.5 Destructive: Delete branch
Offered as a follow-up suggestion after a successful merge (never auto-executed). Same ConfirmModal treatment; preview states branch name + head SHA being deleted.

---

## 5. Notification & Feedback Channel Assignment (§12, narrowed per §0.4)

| Event | Channel | Rationale |
|---|---|---|
| Comment posted | Toast, 4s, undo | safe, recoverable |
| Label toggled | Toast, 4s, undo | safe, recoverable |
| 3 issues failed to load in aggregate view | Banner on IssueList pane, persists until retry succeeds | Partial state, scoped, not safely missable |
| CI check unavailable (runner offline) | Banner on PRDetail, persists, retry action | ongoing condition, not transient |
| Comment post failed | **Inline in the compose card itself** — retry affordance right there, never escalates to toast or banner | see rule below |
| Connection / pending ops / sync age | Status bar, footer right, always visible | ambient global truth |

**Rule for gtui, stricter than the base spec's default table:** toasts are reserved exclusively for safe-action success plus undo. No error, however transient, is ever rendered as a toast — it's inline (scoped to the exact control that failed) or a banner (scoped to the pane), never a fire-and-forget popup. The base spec's "8s for errors" toast variant is not used anywhere in this app.

---

## 6. State Taxonomy Coverage (§9)

**Applicability rule (closing the "applicable" ambiguity from spec review):** a §9 row applies to gtui if the Gitea REST transport, over the operator's WireGuard link, can produce it. This is structural, not a per-screen judgment call.

| State | Applies? | Trigger in gtui |
|---|---|---|
| Loading (cold) | yes | initial repo list fetch on launch |
| Loading (refresh) | yes | background poll of open-issue counts; prior list stays visible |
| Populated | yes | happy path |
| Empty (genuine) | yes | repo with zero open issues |
| Empty (filtered) | yes | label filter yields zero matches |
| Partial [API] | yes | "My Issues" aggregate: one repo's fetch 200s, another's 500s — successful repos render, failed repo gets an inline marker, banner surfaces the count |
| Stale [API] | yes | reconnect after WireGuard drop; cached list shown with age indicator until refetch confirms |
| Error | yes | malformed response; actual Gitea error surfaced verbatim, not "something went wrong" |
| Offline [API] | yes | WireGuard interface down — see §9.1 below |
| Rate-limited [API] | yes | self-hosted instance's configured API rate limit trips a 429 |
| Permission-denied [API] | yes | token lacks collaborator access to a private repo |
| Partial [LOCAL] | **n/a** | no local dataset exists to partially fail |
| Stale [LOCAL] indexing status | **n/a** | no local index; every read is a live or cached-API read, covered by the [API] rows above |

Every [API]-tagged row applies. None is waived by judgment — an API-client app has no state exempt by its own nature. The only waivers here come from the [LOCAL] tag boundary itself, which is exactly the mechanism spec review flagged as missing a definition.

### 6.1 Offline detection specifics
Detected proactively via a periodic heartbeat to the Gitea instance's health endpoint, not purely reactively on request failure — WireGuard can black-hole silently without any request ever timing out on its own schedule. Logged as a deviation in §9.

---

## 7. Determinism Contract (Invariant 5, closing the clock/capability ambiguity)

Snapshot mode receives, as injected fixture inputs — never live reads:

1. A fixed instant substituting for "now." Every relative timestamp (`"2d ago"`, toast countdown, staleness age) computes against this fixture, never `time.Now()` equivalent.
2. A fixed terminal-capability profile (width, color depth) rather than a live terminal query.
3. A fixed API response fixture set rather than a live network call.

No view-layer code path reads the system clock or probes terminal capability directly — both are threaded down from the entrypoint as explicit parameters. This satisfies Invariant 5 without contradicting §6.3's no-color degradation, which is a live-mode-only concern: live mode is free to probe the real terminal; snapshot mode is not.

---

## 8. Keybindings (§17)

**Global (identical across all sbtui apps):**

| Key | Action |
|---|---|
| `q` / `Ctrl+C` | quit |
| `?` | help overlay |
| `:` / `Ctrl+K` | command palette |
| `Tab` / `Shift+Tab` | cycle pane focus |
| `Esc` | cancel / back / close modal |
| `/` | search current pane |

**View-local (IssueList / PRList):**

| Key | Action | Class |
|---|---|---|
| `j`/`k` or arrows | move selection | — |
| `enter` | open detail | — |
| `c` | comment | safe |
| `l` | label picker | safe |
| `x` | close | destructive |
| `r` | reopen | safe |

**PRDetail additionally:**

| Key | Action | Class |
|---|---|---|
| `m` | merge | destructive |
| `d` | delete branch (post-merge prompt) | destructive |
| `g i` | jump to linked issue | — |

**NotificationList:**

| Key | Action | Class |
|---|---|---|
| `enter` | jump to target | — |
| `a` | mark all read | safe (bulk) |

`a` (mark-all-read) is a blanket action, not a scoped selection — it does not enter §15 multi-select mode; that mode is reserved for operator-chosen subsets. Flagged as a judgment call in §10.

---

## 9. Asset Architecture (§22, informative for this app — not a rewrite of the base spec's scope)

```
model/    — Gitea API types, view-state machine, entity/relationship graph
theme/    — token set incl. merged_purple()
layout/   — density-tier calc, panel primitives
views/    — repo_list, issue_list, issue_detail, pr_list, pr_detail,
            notification_list, confirm_modal, command_palette,
            help_overlay, auth_setup
app/      — keybinding dispatch, focus/pane state machine,
            snapshot vs. live entrypoints, transition orchestrator
```

Dependency direction: `entrypoint → app → views → {layout, model, theme}`. Implementation language intentionally unspecified — see Open Items.

---

## Deviations from the base spec

| # | Deviation | Section | Rationale |
|---|---|---|---|
| 1 | Repo creation/deletion, org/team management out of scope | §2 (actions) | Blast radius disproportionate to a terminal quick-glance tool; Gitea's web UI stays authoritative for repo lifecycle |
| 2 | Merge confirmation requires typing the PR number, not a single keypress | §10 | Irreversible on the git graph, triggers CI/deploy side effects — exceeds the spec's minimum confirmation bar deliberately |
| 3 | Toasts never carry error content, at all — no "8s for errors" variant | §12 | All errors render inline (scoped to the failing control) or as a banner (scoped to the pane); narrower than the base spec's default table |
| 4 | Offline detection is proactive (heartbeat) rather than purely reactive-on-failure | §18 | WireGuard can black-hole silently with no failed request to react to |
| 5 | Comment and Branch have no standalone list/detail views | §2 | No cross-repo discovery need at single-operator scale; embedding is a justified reduction, not an oversight |

---

## Open items (unresolved, worth grilling)

- Implementation language/framework — deliberately left open, per spec scope.
- Default merge strategy (squash vs. merge-commit) — squash chosen as the ConfirmModal default above; not validated against actual repo conventions.
- Whether `a` (mark-all-read) belongs in §15 multi-select ceremony instead of a blanket global key — currently treated as a blanket action; could be argued either way.
- Rate-limit countdown UX when the self-hosted instance has no rate limiting configured at all (the common case) — currently undefined whether the Rate-limited row degrades gracefully to "n/a" or is simply never triggered in practice.
