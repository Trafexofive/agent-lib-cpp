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

inline std::vector<TimelineRow> rowsFromAgent(Agent* agent) {
    std::vector<TimelineRow> out;
    if (!agent) {
        out.push_back({TimelineKind::Error, "missing agent", "No agent instance at this path.", false});
        return out;
    }

    // Prefer structured protocolEvents_ (same Thought/Action/Result/Response
    // path as the parent chat — full palette). Events now accumulate across
    // non-ephemeral prompt() calls. Fall back to history_ only when events
    // are empty (e.g. session restore without event replay).
    const auto& events = agent->protocolEvents();
    if (!events.empty()) {
        for (const auto& pe : events) {
            TimelineRow row = rowFromProtocol(pe);
            if (row.kind == TimelineKind::Result && agent->hasSubAgent(row.actionName)) {
                row.drillable = true;
                row.actionType = "agent";
                if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
            } else if (row.kind == TimelineKind::Action && row.actionType == "agent") {
                row.drillable = agent->hasSubAgent(row.actionName);
            }
            out.push_back(std::move(row));
        }
        // Parent missions that only exist in history_ (User/Parent lines) are
        // not protocol events — prepend missing initiator turns from history
        // so the nested chat still shows who spoke, then the structured body.
        std::vector<TimelineRow> inits;
        for (const auto& h : agent->history()) {
            TimelineRow row;
            row.ok = true;
            if (h.rfind("User: ", 0) == 0) {
                row.kind = TimelineKind::User;
                row.title = "user";
                row.body = h.substr(6);
            } else if (h.rfind("Parent(", 0) == 0) {
                row.kind = TimelineKind::User;
                auto close = h.find(')');
                std::string from = "parent";
                std::string body = h;
                if (close != std::string::npos) {
                    from = h.substr(7, close - 7);
                    body = (close + 1 < h.size() && h[close + 1] == ':') ? h.substr(close + 2)
                                                                         : h.substr(close + 1);
                    if (!body.empty() && body[0] == ' ') body = body.substr(1);
                }
                row.title = "parent:" + from;
                row.body = body;
            } else {
                continue;
            }
            if (!row.body.empty()) inits.push_back(std::move(row));
        }
        if (!inits.empty()) {
            // Interleave is hard without timestamps; show initiators first then
            // the full structured event stream (matches multi-prompt continuity).
            inits.insert(inits.end(), out.begin(), out.end());
            out.swap(inits);
        }
        return out;
    }

    // History-only fallback (no structured events).
    for (const auto& h : agent->history()) {
        TimelineRow row;
        row.ok = true;
        if (h.rfind("User: ", 0) == 0) {
            row.kind = TimelineKind::User;
            row.title = "user";
            row.body = h.substr(6);
        } else if (h.rfind("Parent(", 0) == 0) {
            row.kind = TimelineKind::User;
            auto close = h.find(')');
            std::string from = "parent";
            std::string body = h;
            if (close != std::string::npos) {
                from = h.substr(7, close - 7);
                body = (close + 1 < h.size() && h[close + 1] == ':') ? h.substr(close + 2)
                                                                     : h.substr(close + 1);
                if (!body.empty() && body[0] == ' ') body = body.substr(1);
            }
            row.title = "parent:" + from;
            row.body = body;
        } else if (h.rfind("Agent: ", 0) == 0) {
            row.kind = TimelineKind::Response;
            row.title = "response";
            row.body = h.substr(7);
        } else if (h.rfind("System: ", 0) == 0) {
            row.kind = TimelineKind::Log;
            row.title = "system";
            row.body = h.substr(8);
        } else {
            row.kind = TimelineKind::Log;
            row.title = "note";
            row.body = h;
        }
        if (!row.body.empty()) out.push_back(std::move(row));
    }

    if (out.empty()) {
        if (!agent->responseOutput().empty()) {
            out.push_back({TimelineKind::Final, "final", agent->responseOutput(), true});
        } else {
            out.push_back({TimelineKind::Log, "empty", "No history recorded for this agent yet.", true});
        }
    }
    return out;
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

    // Manual drill only (↳ Enter). Never auto-enter on AGENT action —
    // the operator chooses when to open a sub-agent's full chat.
    bool enterSubAgent(const std::string& name) {
        if (name.empty()) return false;
        Agent* parent = currentAgent();
        if (!parent) parent = rootAgent;
        if (!atRoot() && rootAgent && rootAgent->hasSubAgent(name) &&
            (agentPath.size() == 1 || !parent->hasSubAgent(name))) {
            agentPath.clear();
            parent = rootAgent;
        }
        if (!parent) parent = rootAgent;
        if (!parent || !parent->hasSubAgent(name)) {
            status = "no sub-agent: " + name;
            rebuildViews();
            return false;
        }
        if (!agentPath.empty() && agentPath.back() == name) {
            refreshNested();
            return true;
        }
        agentPath.push_back(name);
        {
            auto rows = rowsFromAgent(parent->getSubAgent(name));
            nestedRows.assign(std::make_move_iterator(rows.begin()),
                              std::make_move_iterator(rows.end()));
        }
        selectedBlock = 0;
        timelineFocus = true;
        composer.focused = false;
        rebuildViews();
        return true;
    }

    bool enterSelected() {
        const TimelineRow* row = selectedRow();
        if (!row || !row->drillable) return false;
        return enterSubAgent(row->actionName);
    }

    bool goBack() {
        if (agentPath.empty()) return false;
        agentPath.pop_back();
        if (agentPath.empty()) {
            nestedRows.clear();
        } else {
            Agent* parent = rootAgent;
            for (size_t i = 0; i + 1 < agentPath.size(); ++i) {
                if (!parent) break;
                parent = parent->getSubAgent(agentPath[i]);
            }
            auto rows = rowsFromAgent(parent ? parent->getSubAgent(agentPath.back()) : nullptr);
            nestedRows.assign(std::make_move_iterator(rows.begin()),
                              std::make_move_iterator(rows.end()));
        }
        selectedBlock = 0;
        timelineFocus = true;
        composer.focused = false;
        rebuildViews();
        return true;
    }

    void refreshNested() {
        if (atRoot()) return;
        // Vet-fix: refreshNested runs per child token and rebuilds an entire
        // timeline view from the agent's protocol event log — at 50+ tokens/sec
        // noticeable stalls come from this path, NOT from the apply() route.
        // Cache: budget gated by elapsed wall time; correctness gated by
        // child protocol events growth (size + last child sees a *new*
        // finely-delivered token).
        Agent* cur = currentAgent();
        if (cur) {
            const int64_t now =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            if (now - lastNestedRefreshMs < 100) return;
            lastNestedRefreshMs = now;
        }
        {
            auto rows = rowsFromAgent(cur);
            nestedRows.assign(std::make_move_iterator(rows.begin()),
                              std::make_move_iterator(rows.end()));
        }
        rebuildViews();
    }

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

    static bool isProgressPlaceholder(const ProtocolResult& result) {
        return cortex::mk3::ui::isProgressPlaceholder(result);
    }

    void upsertProtocolRow(size_t index, TimelineRow row) {
        upsertProtocol(index, std::move(row));
    }

    // F3: pure reduction in reduceUiEvent; this method handles product side-effects.
    void apply(const UiEvent& e) {
        TurnState turn;
        turn.running = running;
        turn.done = done;
        turn.failed = failed;
        turn.showRaw = showRaw;
        turn.status = status;
        turn.raw = raw;
        turn.finalText = finalText;
        turn.turnStartMs = turnStartMs;
        turn.lastTurnElapsedMs = lastTurnElapsedMs;

        ReduceEffects fx = reduceUiEvent(*this, turn, e, rootAgent, atRoot(), running, selectedBlock);

        running = turn.running;
        done = turn.done;
        failed = turn.failed;
        status = turn.status;
        raw = turn.raw;
        finalText = turn.finalText;
        turnStartMs = turn.turnStartMs;
        lastTurnElapsedMs = turn.lastTurnElapsedMs;

        // Product side-effects the pure reducer cannot own.
        if (fx.hasNotification) {
            chat::Notification n;
            n.id = fx.notification.id;
            n.source = fx.notification.source;
            n.severity = fx.notification.severity.empty() ? "info" : fx.notification.severity;
            n.title = fx.notification.text;
            n.attempt = fx.notification.attempt;
            n.maxAttempts = fx.notification.maxAttempts;
            n.lifetimeMs = 0;
            notificationStack.push(std::move(n));
        }
        if (fx.hasAskDialog) {
            askDialog = chat::parseDialogState(fx.askDialog.json);
            chat::completeNonInteractiveCards(askDialog);
            askActive = !askDialog.done();
            askInput.value.clear();
            askInput.cursor = 0;
            askInput.focused = true;
            askMultiSelected.clear();
            status = askActive ? "waiting human input" : status;
            if (askActive) openModalFocus("ask");
            else closeModalFocus("ask");
        }
        if (fx.hasAskDialogResult) {
            askActive = false;
            askInput.value.clear();
            askMultiSelected.clear();
            closeModalFocus("ask");
        }

        // Inspector event log (best-effort; not part of pure store).
        if (e.kind == UiEventKind::Protocol) {
            const auto& pe = e.protocol;
            if (pe.kind == ProtocolEventKind::ACTION)
                eventLog.push_back("action " + pe.action.type + ":" + pe.action.name + " #" + pe.action.id);
            else if (pe.kind == ProtocolEventKind::RESULT && !isProgressPlaceholder(pe.result))
                eventLog.push_back(std::string("result ") + (pe.result.ok ? "ok " : "err ") + pe.result.id);
            else if (pe.kind == ProtocolEventKind::THOUGHT)
                eventLog.push_back("thought");
            else if (pe.kind == ProtocolEventKind::RESPONSE)
                eventLog.push_back("response");
        }

        if (fx.needPersist) persistUiTimeline();
        if (fx.needRebuild) rebuildViews();
        if (fx.needRefreshNested) refreshNested();
    }

    // If ask_tool opened a dialog that is already finished (all notes/info, or
    // scene finished cards without the scene holding the bridge), unblock the
    // worker waiting on requestAsk(). Safe no-op when no ask is pending.
    void settleAsk(AgentBridge& bridge) {
        if (!bridge.askPending()) return;
        if (askDialog.cancelled) {
            askActive = false;
            bridge.cancelAsk();
            return;
        }
        if (askDialog.done()) {
            askActive = false;
            bridge.completeAsk(askDialog.results);
            if (!running && status == "waiting human input") status = "ready";
        }
    }

    void drain(AgentBridge& bridge) {
        auto batch = bridge.drain();
        if (!batch.empty()) {
            ++wakeCount;
            batchingEvents = true;
            for (const auto& e : batch) apply(e);
            batchingEvents = false;
            if (viewRebuildPending) rebuildViews();
            // Vet-fix: cap transcript after the batch settles so the cap
            // becomes part of the same tick budget as the rebuild rather
            // than a second O(N) pass mid-tick.
            enforceRowCap();
        }
        settleAsk(bridge);
    }

    void loadSessionRecords(const std::vector<SessionRecord>& records) {
        rootRows.clear();
        for (const auto& record : records) {
            TimelineRow row;
            row.body = record.content;
            row.ok = true;
            switch (record.role) {
                case SessionRecord::USER:
                    row.kind = TimelineKind::User;
                    row.title = "you";
                    break;
                case SessionRecord::AGENT:
                    row.kind = TimelineKind::Response;
                    row.title = "response";
                    break;
                case SessionRecord::TOOL_CALL:
                    row.kind = TimelineKind::Action;
                    row.title = "tool call";
                    break;
                case SessionRecord::TOOL_RESULT:
                    row.kind = TimelineKind::Result;
                    row.title = "tool result";
                    break;
                case SessionRecord::SYSTEM:
                    row.kind = TimelineKind::Log;
                    row.title = "system";
                    break;
            }
            rootRows.push_back(std::move(row));
        }
        timelineState = rootRows.empty() ? PageState::Empty : PageState::Populated;
        selectedBlock = 0;
        markProjFull();
        rebuildViews();
    }

    // Prefer structured ui_timeline when present (exact live parity).
    // Fall back to records projection when older sessions lack the field.
    void loadSessionUi(const Session& session) {
        activeProtocolRows.clear();
        pendingActionIds.clear();
        completedResultIds.clear();
        pendingOps = 0;
        if (!session.uiTimelineJson.empty()) {
            auto rows = deserializeTimeline(session.uiTimelineJson);
            if (!rows.empty()) {
                rootRows.assign(std::make_move_iterator(rows.begin()),
                                std::make_move_iterator(rows.end()));
                timelineState = PageState::Populated;
                selectedBlock = 0;
                markProjFull();
                rebuildViews();
                return;
            }
        }
        loadSessionRecords(session.records);
    }

    // Snapshot live rootRows onto the session file so resume == live.
    // Non-blocking: coalesced async writer (session/perf audit commitAsync).
    // Call persistUiTimelineFlush() on process exit if hard durability required.
    void persistUiTimeline() {
        if (activeSessionId.empty()) return;
        if (session::activeSession().isEphemeral()) return;
        // Keep process-wide SessionRef aligned (single active id).
        session::activeSession().set(activeSessionId, false);
        std::vector<TimelineRow> snap(rootRows.begin(), rootRows.end());
        std::string json = serializeTimeline(snap);
        if (json.empty() || json == "[]") return;
        static std::atomic<uint64_t> gen{1};
        session::UiTimelineCommit c;
        c.sessionId = activeSessionId;
        c.baseDir = rootAgent ? rootAgent->sessionMgr().baseDir() : std::string{};
        c.uiTimelineJson = std::move(json);
        c.agentName = agentName;
        c.model = agentModel;
        c.provider = agentProvider;
        c.generation = gen.fetch_add(1, std::memory_order_relaxed);
        session::AsyncUiTimelineWriter::instance().enqueue(std::move(c));
    }

    void persistUiTimelineFlush() {
        session::AsyncUiTimelineWriter::instance().flush();
    }

    void clearTranscript() {
        rootRows.clear();
        nestedRows.clear();
        activeProtocolRows.clear();
        pendingActionIds.clear();
        completedResultIds.clear();
        pendingOps = 0;
        selectedBlock = 0;
        markProjFull();
        rebuildViews();
    }

    void appendNotice(const std::string& title, const std::vector<std::string>& lines) {
        TimelineRow row;
        row.kind = TimelineKind::Log;
        row.title = title;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) row.body += "\n";
            row.body += lines[i];
        }
        pushRow(std::move(row));
    }

    bool historyPrevious() {
        if (promptHistory.empty() || running || !atRoot()) return false;
        if (promptHistoryIndex >= static_cast<int>(promptHistory.size())) {
            promptHistoryDraft = composer.value;
            promptHistoryIndex = static_cast<int>(promptHistory.size());
        }
        if (promptHistoryIndex <= 0) return false;
        --promptHistoryIndex;
        composer.value = promptHistory[static_cast<size_t>(promptHistoryIndex)];
        composer.cursor = static_cast<int>(composer.value.size());
        return true;
    }

    bool historyNext() {
        if (promptHistory.empty() || running || !atRoot()) return false;
        if (promptHistoryIndex >= static_cast<int>(promptHistory.size())) return false;
        ++promptHistoryIndex;
        composer.value = promptHistoryIndex == static_cast<int>(promptHistory.size())
                             ? promptHistoryDraft
                             : promptHistory[static_cast<size_t>(promptHistoryIndex)];
        composer.cursor = static_cast<int>(composer.value.size());
        return true;
    }

    bool submitComposer() {
        if (running || !atRoot()) return false;
        std::string text = composer.value;
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
        size_t start = 0;
        while (start < text.size() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n')) ++start;
        text = text.substr(start);
        if (text.empty()) return false;
        // Vet-fix: arm an ephemeral session id at first turn-in-chat so
        // the work the operator just typed lands on disk. Phase 1 removed
        // auto-mint on bare TUI launches, which kept phantom file pairs
        // out of the Sessions page — but it also meant a prompt typed
        // into the chat produced NO file, because activeSessionId stayed
        // empty even after work had been done. Arm lazily here: the
        // first non-empty submit of a session-bare chat allocates a new
        // id; subsequent turns reuse it. Sessions page learns of the
        // file when the worker saves at TurnDone or atexit flushes.
        if (activeSessionId.empty()) {
            // Unified mint (F6): same scheme as CLI / hub create / hub fork.
            // Arm unconditionally; --no-session suppresses disk at flush time.
            activeSessionId = session::mintSessionId();
            session::activeSession().set(activeSessionId, session::activeSession().isEphemeral());
            dashboard.notice = std::string("armed ") + activeSessionId;
            // Vet-fix: seed the typed prompt into the agent's history_
            // and persist immediately. Otherwise the operator's typed
            // prompt disappears when the TUI exits between submit and
            // prompt() landing a record — and `recover session` shows
            // an empty chat. seedUserPrompt is idempotent with prompt()'s
            // own push (it dedupes by trailing-equality), so the worker's
            // subsequent save still produces a clean record set.
            if (rootAgent && !session::activeSession().isEphemeral()) {
                rootAgent->seedUserPrompt(text);
                rootAgent->saveSession(activeSessionId);
            }
        }
        pendingSubmit = text;
        if (promptHistory.empty() || promptHistory.back() != text) promptHistory.push_back(text);
        promptHistoryIndex = static_cast<int>(promptHistory.size());
        promptHistoryDraft.clear();
        pushRow({TimelineKind::User, "you", text, true});
        composer.value.clear();
        composer.cursor = 0;
        composer.scroll_row = 0;
        rebuildViews();
        return true;
    }
};

}  // namespace cortex::mk3::ui

// Out-of-line projection methods (ShellModel must be complete).
#include "src/ui/model/timeline_projection.hpp"
