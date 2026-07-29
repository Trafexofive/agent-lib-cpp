#pragma once
// Domain model for the inkcell AgentShell. Drawing stays out of this file.
// Includes timeline block focus + nested sub-agent history drill-down.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <deque>
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
    // When true, long block bodies are capped (pi-like truncation) with a
    // "… N more lines" note. Toggle via /truncate or CLI --[no-]truncate.
    bool truncateBodies = true;
    static constexpr int kMaxBodyLines = 50;
    // tokenBytes/actionCount/resultCount/pendingOps live in TimelineStore
    int wakeCount = 0;
    int routeTicks = 0;
    std::string activePage = "Agent";
    std::string pendingSubmit;
    PendingRoute pendingRoute = PendingRoute::None;
    // Hub Enter on a launchable agent sets this path; REPL tick hot-swaps the
    // live Agent then routes to the chat scene. Cleared after attempt.
    std::string pendingLaunchManifest;
    std::string launchError;  // last hot-swap failure (surfaced on Home/app bar)
    // Hub Enter on kind=workflow — REPL tick runs WorkflowEngine on a worker.
    std::string pendingRunWorkflow;
    bool pendingStopWorkflow = false;
    // Shared live run hub (worker writes, UI snapshots).
    model::WorkflowRunHub workflowRun;
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
    // inkcell FocusManager dogfood (composer/timeline/palette/ask layers).
    // timelineFocus remains the product boolean for projection; keep in sync via focus*().
    inkcell::FocusManager focus;
    // Maps visible block index -> rootRows/nested row index
    std::vector<int> blockRowIndex;
    // nestedRows, activeProtocolRows, pendingActionIds, completedResultIds,
    // projDirtyFrom, rootRowLineStart live in TimelineStore
    bool batchingEvents = false;
    bool viewRebuildPending = false;
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

    void applyRowBans(TimelineRow row) {
        // Nested drill is a historical view; live updates always land on root.
        appendRoot(std::move(row));
        if (!atRoot()) return;
        if (running) selectedBlock = std::max(0, countFocusable(rootRows, showThoughts) - 1);
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
        selectedBlock = std::max(0, std::min(n - 1, selectedBlock + delta));
        markProjFull();  // › marker moves between blocks
        rebuildViews();
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
        timelineFocus = true;
        composer.focused = false;
        focus.focus("timeline");
        if (selectedBlock < 0) selectedBlock = 0;
        markProjFull();  // › selection markers on all blocks
        rebuildViews();
    }

    void focusComposer() {
        if (!atRoot()) return;  // nested views are browse-only
        timelineFocus = false;
        composer.focused = true;
        focus.focus("composer");
        markProjFull();  // clear › markers
        rebuildViews();
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
