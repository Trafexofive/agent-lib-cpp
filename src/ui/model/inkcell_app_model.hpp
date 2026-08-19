#pragma once
// Domain model for the inkcell AgentShell. Drawing stays out of this file.
// Includes timeline block focus + nested sub-agent history drill-down.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "inkcell/widgets/scroll_view.hpp"
#include "inkcell/widgets/textarea.hpp"
#include "src/core/agent.hpp"
#include "src/protocol/noise.hpp"
#include "src/session/controller.hpp"
#include "src/ui/chat/notification.hpp"
#include "src/ui/chat/ask_dialog_model.hpp"
#include "src/ui/chat/block_reader.hpp"
#include "src/ui/chat/chat_footer.hpp"
#include "src/ui/chat/transcript_cache.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/model/dashboard_model.hpp"
#include "src/ui/model/timeline_codec.hpp"  // TimelineKind/Row + codec (F2)
#include "src/ui/model/pending_route.hpp"   // PendingRoute (F6)
#include "inkcell/focus.hpp"
#include "src/ui/model/timeline_store.hpp"  // TimelineStore (F3)
#include "src/ui/model/event_reducer.hpp"   // reduceUiEvent (F3)
#include "src/ui/model/workflow_run_model.hpp"
#include "src/ui/text/sanitize.hpp"         // sanitizeForDisplay (F2)
#include "src/ui/bridge/agent_bridge.hpp"

namespace cortex::mk3::ui {

struct InkcellAppConfig {
    std::string agentName;
    std::string provider;
    std::string model;
    std::string manifestPath;   // active agent.yml (if any)
    std::string manifestDir;    // discovery root override (--manifest-dir or derived)
    std::string harnessPath;
    std::string systemPromptPath;
    std::string personaPath;
    std::string sessionId;
    int toolCount = 0;
    int feedCount = 0;
    int relicCount = 0;
    int subAgentCount = 0;
    // Lifecycle flags (orthogonal):
    //   noSession  → don't load/save session records
    //   ephemeral  → exit the TUI when the agent turn finishes
    bool noSession = false;
    bool ephemeral = false;
    // Optional seed prompt for interactive REPL (-p without --ephemeral).
    std::string initialPrompt;
    // Bare `-m` (no name): open the app directly on the manifests browser.
    bool startAtManifests = false;
    // Chat render modifiers (orthogonal to -p / session / lifecycle).
    bool showThoughts = true;
    bool truncateBodies = true;
};

inline bool snapshotMode() {
    const char* s = std::getenv("MK3_TUI_SNAPSHOT");
    return (s && s[0]) || !isatty(STDOUT_FILENO);
}

inline std::string nonempty(const std::string& value, const std::string& fallback) {
    return value.empty() ? fallback : value;
}

// F3: ShellModel is TimelineStore + chrome/turn/session orchestration.
// Row storage, protocol map, caps, sanitize append live in TimelineStore.
// apply() delegates pure event reduction to reduceUiEvent().
struct ShellModel : TimelineStore {
    chat::NotificationStack notificationStack;
    std::vector<std::string> eventLog;
    std::string raw;
    std::string finalText;
    std::string status = "idle";
    // timelineState lives in TimelineStore
    bool running = false;
    int64_t turnStartMs = 0;  // steady_clock ms when the current turn started; 0 when idle. Drives live metrics.
    int64_t lastTurnElapsedMs = 0;  // duration of the most recently completed turn; persists after the turn ends for the "last" summary.
    int64_t lastNestedRefreshMs = 0;  // vet-fix: rate-limit refreshNested() so live sub-agent nesting doesn't rebuild O(protocolEvents) per token.
    bool done = false;
    bool failed = false;
    bool showThoughts = true;
    bool showRaw = false;
    // Vet-fix: chat-body field underlay opt-in. Off by default — the chat
    // surface stays crisp unless the operator opts in. Hub field on/off is
    // shared via gfx:: and this is the chat-side gate.
    bool chatFieldEnabled = false;
    // When true, streaming keeps selection/scroll on the live edge.
    bool autoFollowLive = true;
    // Hub chrome prefs (Settings · CHROME).
    // zenMode: pill auto-hides after navPillHideMs of idle nav; reappears on g/s/a/?/dock.
    bool zenMode = false;
    // Master pill switch — false removes the dock (stage uses full bottom padding).
    bool navPillEnabled = true;
    // Auto-hide delay in zen mode (ms). 0 = never auto-hide.
    int navPillHideMs = 3000;
    // Session CWD applied on create/resume (Settings · CWD). Empty = process CWD.
    std::string sessionCwd;
    // CWD the binary was launched from — included in the cycle so the
    // operator can return to their project after cycling through HOME.
    std::string launchCwd;
    // When ON, persisted sessionCwd is honored at app launch (process chdir).
    // When OFF (default), launch dir is used; CWD is per-session only.
    bool rememberLastCwd = false;
    // When ON, CWD change leaves the live worker and session intact —
    // just chdir the process. When OFF (default), live session is killed
    // and its file deleted (the historical "exit and reopen" semantics).
    bool keepLiveOnCwdChange = false;
    // OFF = Sessions page/resume scoped to current CWD; ON = all projects.
    bool globalSessions = false;
    // Settings · DEV MODE — gates debug slash cmds (/export-dump, /dump-prompt…).
    bool uiDevMode = false;
    // When true, long block bodies are capped (pi-like truncation) with a
    // "… N more lines" note. Toggle via /truncate or CLI --[no-]truncate.
    bool truncateBodies = true;
    // Action (input) / result (output) body presentation: json | yaml | raw.
    // Settings · CHAT · INPUT FMT / OUTPUT FMT carousels.
    BodyRenderMode actionBodyMode = BodyRenderMode::PrettyJson;
    BodyRenderMode resultBodyMode = BodyRenderMode::PrettyJson;
    // Cyclable footer under the prompt (Ctrl-F). See chat_footer.hpp.
    chat::ChatFooterPane chatFooterPane = chat::ChatFooterPane::Live;
    // 0 stream · 1 compact · 2 graph — Ctrl-O cycles
    int chatBodyMode = 0;
    static constexpr int kMaxBodyLines = 50;
    // tokenBytes/actionCount/resultCount/pendingOps live in TimelineStore
    int wakeCount = 0;
    int routeTicks = 0;
    std::string activePage = "Agent";
    std::string pendingSubmit;
    // /continue — kick another turn with empty user input (no YOU row).
    bool pendingContinue = false;
    // Steer text held when running but rootAgent not yet wired (rare).
    std::string pendingSteerBuffer;

    // Fullscreen child TUI (art / $EDITOR). Wired from repl → Engine.
    std::function<void()> suspendTui;
    std::function<void()> resumeTui;
    PendingRoute pendingRoute = PendingRoute::None;
    // Hub Enter on a launchable agent sets this path; REPL tick hot-swaps the
    // live Agent then routes to the chat scene. Cleared after attempt.
    std::string pendingLaunchManifest;
    // After hub resume requests a manifest hot-swap, load this session onto the
    // newly built agent (repl tick clears both).
    std::string pendingResumeSessionId;
    std::string launchError;  // last hot-swap failure (surfaced on Home/app bar)
    // Hub Enter on kind=workflow — REPL tick runs WorkflowEngine on a worker.
    std::string pendingRunWorkflow;
    bool pendingStopWorkflow = false;
    // Shared live run hub (worker writes, UI snapshots).
    model::WorkflowRunHub workflowRun;
    // ── Tool scene state ───────────────────────────────────────────────
    // Hub Enter on kind=tool routes to scenes::ToolScene (full UX: input
    // form auto-gen from input_schema + streaming output + run history).
    // Last run = the currently-rendering result. toolHistory = persistent
    // per-tool log the operator can scroll + copy from. toolRunsBusy guards
    // the worker thread against re-entrant ↵ presses while a run is live.
    struct ToolRunRecord {
        std::string toolName;
        std::string paramsJson;   // frozen at run-time for reproducibility
        std::string output;       // streaming output buffer
        bool success = false;
        std::string error;
        int64_t elapsedMs = 0;
        int64_t timestampMs = 0;  // steady_clock ms at run start
        bool running = false;     // true while the worker thread is alive
    };
    ToolRunRecord lastToolRun;
    std::vector<ToolRunRecord> toolHistory;  // capped to 32 entries (FIFO)
    bool toolRunsBusy = false;
    // The path of the tool manifest currently being inspected (set by
    // ToolScene::on_enter, used in the topbar + status). Empty when the
    // tool scene isn't active.
    std::string activeToolManifestPath;
    std::string activeToolName;
    std::string activeRelicManifestPath;
    std::string activeRelicName;
    std::string activeWorkflowManifestPath;
    std::string activeWorkflowName;
    // Live tool worker cancel signal — flipped by the scene on Esc/x,
    // checked by the worker between output chunks.
    bool toolCancelRequested = false;
    std::string activeSessionId;
    // Agent display identity for the chat transcript labels. The assistant's own
    // turns (Response/Final) are labeled with agentName + agentModel/agentProvider
    // instead of the generic "CORTEX" sentinel, and subagent Action turns show
    // the subagent name + metadata. Set once by initializeChatModel from the
    // InkcellAppConfig (manifest-resolved).
    std::string agentName;
    std::string agentModel;
    std::string agentProvider;
    std::string activeManifestPath;  // live agent.yml — updated on hub launch
    model::DashboardState dashboard;
    inkcell::widgets::TextAreaState composer;
    std::vector<std::string> promptHistory;
    int promptHistoryIndex = 0;
    std::string promptHistoryDraft;

    bool helpVisible = false;
    // Enter-on-block full-text reader (Esc/Backspace closes).
    chat::BlockReaderState blockReader;
    components::CmdPalette cmdPalette;
    std::string pendingPaletteAction;  // set on palette Enter; scenes execute
    bool askActive = false;
    chat::DialogState askDialog;
    inkcell::widgets::TextAreaState askInput;
    std::set<int> askMultiSelected;

    // Readline-style slash completion (LCP then cycle).
    std::vector<std::string> tabMatches;
    int tabMatchIndex = -1;
    std::string tabStem;  // prefix used to build tabMatches

    void clearTabCompletion() {
        tabMatches.clear();
        tabMatchIndex = -1;
        tabStem.clear();
    }
    mutable inkcell::widgets::ScrollViewState transcriptView;
    mutable inkcell::widgets::ScrollViewState inspectorView;

    // Focus + history navigation
    Agent* rootAgent = nullptr;
    std::vector<std::string> agentPath;  // nested sub-agent names from root
    int selectedBlock = 0;               // index into visible focusable blocks
    bool timelineFocus = false;          // false = composer owns keys
    // Set by selectDelta/selectEdge/focusTimeline; consumed once in finishRebuildScroll
    // so ensureSelectionVisible does not fight free scroll (Ctrl-J/K) every frame.
    bool selectionNavPending = false;
    // inkcell FocusManager dogfood (composer/timeline/palette/ask layers).
    // timelineFocus remains the product boolean for projection; keep in sync via focus*().
    inkcell::FocusManager focus;
    // Maps visible block index -> rootRows/nested row index
    std::vector<int> blockRowIndex;
    // nestedRows, activeProtocolRows, pendingActionIds, completedResultIds,
    // projDirtyFrom, rootRowLineStart live in TimelineStore
    bool batchingEvents = false;
    bool viewRebuildPending = false;
    // One-shot: next rebuildViews must take full path (trunc/fmt/selection chrome).
    // Public so scenes can force full after filter toggles (thoughts/raw).
    bool forceFullProject_ = false;
    uint64_t viewRebuildCount = 0;
    uint64_t transcriptVersion = 0;
    mutable chat::TranscriptWrapCache transcriptWrapCache;

    ShellModel() {
        composer.focused = true;
        composer.value.clear();
        focus.add("composer");
        focus.add("timeline");
        focus.focus("composer");
        rebuildViews();
    }

    void setRootAgent(Agent* agent) {
        rootAgent = agent;
        // Align shell identity with the live Agent config (manifest wins).
        // Prevents header/footer showing stale session/cli provider·model.
        if (rootAgent) {
            const auto& c = rootAgent->config();
            if (!c.name.empty()) agentName = c.name;
            if (!c.model.empty()) agentModel = c.model;
            if (!c.provider.empty()) agentProvider = c.provider;
        }
        reannotateDrillable();
        rebuildViews();
    }

    void routeTo(std::string page) {
        activePage = std::move(page);
        routeTicks = snapshotMode() ? 0 : 10;
    }

    void tickRoute() {
        if (routeTicks > 0) --routeTicks;
    }

    int transitionInset() const {
        if (snapshotMode() || routeTicks <= 0) return 0;
        return std::max(0, routeTicks / 3);
    }

    bool atRoot() const { return agentPath.empty(); }

    // Returns the body of the most recent Response timeline row (the
    // streaming/final LLM response text), or empty when there is none yet.
    // The dashboard preview line truncates this for display while a turn is
    // running. Scans backward so the latest response wins.
    std::string lastResponseBody() const {
        for (auto it = rootRows.rbegin(); it != rootRows.rend(); ++it) {
            if (it->kind == TimelineKind::Response) return it->body;
        }
        return {};
    }

    Agent* currentAgent() const {
        if (!rootAgent) return nullptr;
        Agent* cur = rootAgent;
        for (const auto& name : agentPath) {
            cur = cur->getSubAgent(name);
            if (!cur) return nullptr;
        }
        return cur;
    }

    const std::deque<TimelineRow>& activeRows() const {
        return atRoot() ? rootRows : nestedRows;
    }

    std::string breadcrumb() const {
        std::string path = nonempty(rootAgent ? rootAgent->name() : "", "root");
        for (const auto& name : agentPath) path += " / " + name;
        return path;
    }

    void pushRow(TimelineRow row) {
        applyRowBans(std::move(row));
    }

    // Cap is TimelineStore::kRootRowCap; rebuild when rows drop.
    void enforceRowCap() {
        if (TimelineStore::enforceRowCap(selectedBlock) > 0) rebuildViews();
    }

    // True when selection is on the live edge (last focusable block).
    bool selectionOnLiveEdge() const {
        int n = countFocusable(rootRows, showThoughts);
        if (n <= 0) return true;
        return selectedBlock >= n - 1;
    }

    // Live-stream follow: stick_bottom is the lock. Selecting the last block
    // sets it; free-scroll (Ctrl-J/K) clears it even if selection stays on last.
    // Do NOT re-lock from selectionOnLiveEdge alone — that fought scroll unlock.
    void followLiveEdgeIfLocked() {
        if (!autoFollowLive) return;
        if (!running) return;
        if (!transcriptView.stick_bottom) return;
        int last = std::max(0, countFocusable(activeRows(), showThoughts) - 1);
        // When a new focusable block appears, selection jumps to the new last.
        // Incremental project only rewrites the dirty tail — the previous last
        // keeps its › chrome → double highlight until j/k forces full rebuild.
        if (selectedBlock != last) {
            selectedBlock = last;
            markProjFull();
        } else {
            selectedBlock = last;
        }
    }

    void applyRowBans(TimelineRow row) {
        // Nested drill is a historical view; live updates always land on root.
        appendRoot(std::move(row));
        if (!atRoot()) return;
        followLiveEdgeIfLocked();
        rebuildViews();
    }

    // Prefer TimelineStore::countFocusable; keep method for call-site compatibility.
    template <typename RowRange>
    static int countFocusable(const RowRange& rows, bool showThoughtsFlag = true) {
        return TimelineStore::countFocusable(rows, showThoughtsFlag);
    }

    void setStreamProgress(int bytes) {
        if (!atRoot()) {
            tokenBytes = bytes;
            return;
        }
        TimelineStore::setStreamProgress(bytes);
        rebuildViews();
    }

    // markProjDirty / markProjFull inherited from TimelineStore

    // Projection — definitions in timeline_projection.hpp (F4).
    bool projectOneRow(const TimelineRow& row, int ri, int& focusIdx, const std::string& scopeName,
                       bool recordRootLineStart);
    void rebuildInspector();
    void finishRebuildScroll();
    void rebuildViewsFull();
    bool tryRebuildViewsIncremental();
    void rebuildViews();
    void ensureSelectionVisible();

    void selectDelta(int delta) {
        int n = static_cast<int>(blockRowIndex.size());
        if (n <= 0) return;
        // Selection paint keys off historyFocused — stay in timeline mode.
        timelineFocus = true;
        composer.focused = false;
        selectedBlock = std::max(0, std::min(n - 1, selectedBlock + delta));
        // Lock live-follow only when selection sits on the last block.
        transcriptView.stick_bottom = (selectedBlock >= n - 1);
        selectionNavPending = true;
        // Full reproject: › chrome moves on two rows; incremental dirty left
        // stale markers and fought wrap-cache. Select is rare vs stream.
        markProjFull();
        rebuildViews();
    }

    // Jump selection to first/last focusable block (gg / G).
    void selectEdge(bool toEnd) {
        int n = static_cast<int>(blockRowIndex.size());
        if (n <= 0) return;
        timelineFocus = true;
        composer.focused = false;
        selectedBlock = toEnd ? n - 1 : 0;
        transcriptView.stick_bottom = toEnd;
        selectionNavPending = true;
        markProjFull();
        rebuildViews();
    }

    void toggleSelectedCollapsed() {
        if (selectedBlock < 0 || selectedBlock >= static_cast<int>(blockRowIndex.size())) return;
        int ri = blockRowIndex[static_cast<size_t>(selectedBlock)];
        auto& rows = atRoot() ? rootRows : nestedRows;
        if (ri < 0 || ri >= static_cast<int>(rows.size())) return;
        rows[static_cast<size_t>(ri)].collapsed = !rows[static_cast<size_t>(ri)].collapsed;
        markProjFull();
        transcriptWrapCache.invalidate();
        ++transcriptVersion;
        forceFullProject_ = true;
        rebuildViews();
    }

    // Toggle body truncation and force full wrap-cache rebuild (Ctrl-O / /truncate).
    void toggleTruncateBodies() {
        truncateBodies = !truncateBodies;
        // Truncation changes every projected body — incremental tail rewrite
        // leaves the stable prefix at the old density (Ctrl-O mid-stream bug).
        markProjFull();
        transcriptWrapCache.invalidate();
        ++transcriptVersion;
        // If drain is batching, rebuildViews defers — keep force-full flag so
        // the post-batch rebuild cannot take the incremental path.
        forceFullProject_ = true;
        rebuildViews();
    }

    // Cycle json → yaml → raw for action bodies (Settings · INPUT FMT).
    void cycleActionBodyMode(int dir = 1) {
        int m = static_cast<int>(actionBodyMode);
        m = (m + (dir >= 0 ? 1 : 2)) % 3;
        actionBodyMode = static_cast<BodyRenderMode>(m);
        markProjFull();
        forceFullProject_ = true;
        transcriptWrapCache.invalidate();
        ++transcriptVersion;
        rebuildViews();
    }

    // Cycle json → yaml → raw for result bodies (Settings · OUTPUT FMT).
    void cycleResultBodyMode(int dir = 1) {
        int m = static_cast<int>(resultBodyMode);
        m = (m + (dir >= 0 ? 1 : 2)) % 3;
        resultBodyMode = static_cast<BodyRenderMode>(m);
        markProjFull();
        forceFullProject_ = true;
        transcriptWrapCache.invalidate();
        ++transcriptVersion;
        rebuildViews();
    }

    // Prompt-history push: no consecutive duplicate of the exact same entry.
    void pushPromptHistory(const std::string& text) {
        if (text.empty()) return;
        if (!promptHistory.empty() && promptHistory.back() == text) return;
        promptHistory.push_back(text);
        promptHistoryIndex = static_cast<int>(promptHistory.size());
        promptHistoryDraft.clear();
    }

    // Yank selected block body (easy kinds). Returns empty if nothing to copy.
    std::string yankSelectedBody() const {
        const TimelineRow* row = selectedRow();
        if (!row) return {};
        // Prefer body; fall back to title for thin rows.
        if (!row->body.empty()) return row->body;
        return row->title;
    }

    const TimelineRow* selectedRow() const {
        if (selectedBlock < 0 || selectedBlock >= static_cast<int>(blockRowIndex.size())) return nullptr;
        int ri = blockRowIndex[static_cast<size_t>(selectedBlock)];
        const auto& rows = activeRows();
        if (ri < 0 || ri >= static_cast<int>(rows.size())) return nullptr;
        return &rows[static_cast<size_t>(ri)];
    }
    // Nested drill — definitions in shell_nav_session.hpp (F4b).
    bool enterSubAgent(const std::string& name);
    bool enterSelected();
    bool goBack();
    void refreshNested();

    void focusTimeline() {
        // Preserve scroll + stick across focus switches. Do not force unlock
        // stick_bottom — that killed live-follow and broke mid-run nav restore.
        const int savedOff = transcriptView.offset;
        const bool savedStick = transcriptView.stick_bottom;
        timelineFocus = true;
        composer.focused = false;
        focus.focus("timeline");
        if (selectedBlock < 0) selectedBlock = 0;
        selectionNavPending = false;  // don't snap viewport on mere focus flip
        markProjFull();
        rebuildViews();
        transcriptView.stick_bottom = savedStick;
        if (!savedStick) {
            transcriptView.offset = savedOff;
            transcriptView.clamp();
        } else {
            transcriptView.scroll_to_end();
        }
    }

    void focusComposer() {
        const int savedOff = transcriptView.offset;
        const bool savedStick = transcriptView.stick_bottom;
        timelineFocus = false;
        composer.focused = true;
        focus.focus("composer");
        selectionNavPending = false;
        markProjFull();
        rebuildViews();
        transcriptView.stick_bottom = savedStick;
        if (!savedStick) {
            transcriptView.offset = savedOff;
            transcriptView.clamp();
        } else {
            transcriptView.scroll_to_end();
        }
    }

    void requestRoute(PendingRoute r) { pendingRoute = r; }
    void clearRoute() { pendingRoute = PendingRoute::None; }

    // Modal focus layers (inkcell FocusManager stack). Esc / dismiss pops.
    void openModalFocus(const std::string& name) {
        if (focus.in_modal() && focus.layer_name() == name) return;
        // Nested modal: push on top (ask over palette is fine).
        focus.push_layer(name, {name}, name);
    }
    void closeModalFocus(const std::string& name) {
        if (focus.layer_name() == name) {
            focus.pop_layer();
            return;
        }
        // Drop through to named layer if buried.
        if (focus.in_modal()) focus.pop_to(name);
    }
    bool modalFocusIs(const std::string& name) const {
        return focus.in_modal() && focus.layer_name() == name;
    }

    // Events — definitions in shell_events.hpp (F7).
    static bool isProgressPlaceholder(const ProtocolResult& result);
    void upsertProtocolRow(size_t index, TimelineRow row);
    void apply(const UiEvent& e);
    void settleAsk(AgentBridge& bridge);
    void drain(AgentBridge& bridge);

    // Session load/persist — definitions in shell_nav_session.hpp (F4b).
    void loadSessionRecords(const std::vector<SessionRecord>& records);
    void loadSessionUi(const Session& session);
    void reannotateDrillable();  // re-bind ↳ enter after restore / setRootAgent
    void persistUiTimeline();
    void persistUiTimelineFlush();
    void clearTranscript();

    // Composer — definitions in shell_composer.hpp (F7).
    void appendNotice(const std::string& title, const std::vector<std::string>& lines);
    bool historyPrevious();
    bool historyNext();
    bool submitComposer();
};

}  // namespace cortex::mk3::ui

// Out-of-line projection methods (ShellModel must be complete).
#include "src/ui/model/timeline_projection.hpp"
#include "src/ui/model/shell_nav_session.hpp"
#include "src/ui/model/shell_events.hpp"
#include "src/ui/model/shell_composer.hpp"
