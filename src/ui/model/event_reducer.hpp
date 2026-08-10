#pragma once
// =============================================================================
// EventReducer — UiEvent → TimelineStore + turn state (foundation F3).
// Pure w.r.t. Surface/projection. Optional Agent* only for drillable labels.
// Ask/Notification side-effects returned as flags for the host to handle.
// =============================================================================

#include <chrono>
#include <string>

#include "src/core/agent.hpp"  // Agent* for drillable only; prefer events.hpp later via port
#include "src/protocol/events.hpp"
#include "src/ui/bridge/ui_event.hpp"
#include "src/ui/model/timeline_codec.hpp"
#include "src/ui/model/timeline_store.hpp"
#include "src/ui/text/sanitize.hpp"

namespace cortex::mk3::ui {

struct TurnState {
    bool running = false;
    bool done = false;
    bool failed = false;
    bool showRaw = false;
    std::string status = "idle";
    std::string raw;
    std::string finalText;
    int64_t turnStartMs = 0;
    int64_t lastTurnElapsedMs = 0;
};

struct ReduceEffects {
    bool needRebuild = false;
    bool needRefreshNested = false;
    bool needPersist = false;
    // Host must handle these (not pure store).
    bool hasNotification = false;
    UiEvent notification;  // copy of Notification event
    bool hasAskDialog = false;
    UiEvent askDialog;
    bool hasAskDialogResult = false;
};

inline bool isProgressPlaceholder(const ProtocolResult& result) {
    return result.elapsedMs == 0.0 && result.summary.find(" is running…") != std::string::npos;
}

inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Apply one UiEvent. atRoot controls nested refresh vs root rebuild.
// selectedBlock adjusted when streaming sticks to bottom.
inline ReduceEffects reduceUiEvent(TimelineStore& store, TurnState& turn, const UiEvent& e,
                                   Agent* rootAgent, bool atRoot, bool runningStickSelect,
                                   int& selectedBlock) {
    ReduceEffects fx;
    switch (e.kind) {
        case UiEventKind::Status: {
            bool isRunningStatus = e.text.find("running") != std::string::npos;
            if (isRunningStatus) {
                store.clearProtocolEpoch();
                store.actionCount = 0;
                store.resultCount = 0;
                store.tokenBytes = 0;
                turn.raw.clear();
                turn.done = false;
                turn.failed = false;
                // Always (re)arm the live timer if missing — fixes sticky 0ms
                // when running was already true without a start stamp.
                if (!turn.running || turn.turnStartMs <= 0) turn.turnStartMs = nowMs();
                store.timelineState = PageState::Loading;
            }
            turn.status = e.text;
            turn.running = isRunningStatus;
            break;
        }
        case UiEventKind::Log:
            store.appendRoot({TimelineKind::Log, "log", e.text, true});
            if (atRoot) fx.needRebuild = true;
            break;
        case UiEventKind::Error: {
            // Operator cancel surfaces as cancelled, not a hard error row.
            const bool isCancel =
                e.text == "cancelled" || e.text == "[cancelled]" ||
                e.text.find("cancelled") != std::string::npos ||
                e.text.find("ABORTED_BY_CALLBACK") != std::string::npos ||
                e.text.find("Operation was aborted") != std::string::npos;
            turn.running = false;
            if (turn.turnStartMs > 0) {
                turn.lastTurnElapsedMs = nowMs() - turn.turnStartMs;
                turn.turnStartMs = 0;
            }
            store.pendingActionIds.clear();
            store.pendingOps = 0;
            store.actionCount = 0;
            if (isCancel) {
                turn.failed = false;
                turn.status = "cancelled";
                store.timelineState = PageState::Populated;
                // No Error row — cancel is an operator action, not a fault.
            } else {
                turn.failed = true;
                turn.status = "error";
                store.timelineState = PageState::Error;
                store.appendRoot({TimelineKind::Error, "error", e.text, false});
            }
            if (atRoot) fx.needRebuild = true;
            break;
        }
        case UiEventKind::Notification:
            fx.hasNotification = true;
            fx.notification = e;
            break;
        case UiEventKind::Token:
            if (!turn.running) {
                turn.running = true;
                if (turn.turnStartMs <= 0) turn.turnStartMs = nowMs();
                if (turn.status.empty() || turn.status == "ready" || turn.status == "idle")
                    turn.status = "agent running";
            }
            turn.raw += e.text;
            store.tokenBytes += static_cast<int>(e.text.size());
            if (turn.showRaw) {
                std::string sanitized = sanitizeForDisplay(e.text);
                for (auto& line : splitDisplayLines(sanitized))
                    store.appendRoot({TimelineKind::Stream, "raw", line, true});
                if (atRoot) fx.needRebuild = true;
            }
            if (!atRoot) fx.needRefreshNested = true;
            break;
        case UiEventKind::Protocol: {
            if (!turn.running) {
                turn.running = true;
                if (turn.turnStartMs <= 0) turn.turnStartMs = nowMs();
                if (turn.status.empty() || turn.status == "ready" || turn.status == "idle")
                    turn.status = "agent running";
            }
            const auto& pe = e.protocol;
            if (pe.kind == ProtocolEventKind::RETRY) {
                store.clearProtocolEpoch();
                break;
            }
            TimelineRow row = rowFromProtocol(pe);
            if (row.kind == TimelineKind::Log && row.title == "noise") break;

            if (pe.kind == ProtocolEventKind::ACTION) {
                if (store.pendingActionIds.insert(pe.action.id).second) {
                    ++store.actionCount;
                    store.pendingOps = static_cast<int>(store.pendingActionIds.size());
                }
                if (row.actionType == "agent" && rootAgent && rootAgent->hasSubAgent(row.actionName)) {
                    row.drillable = true;
                    if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
                } else if (row.actionType != "agent") {
                    row.drillable = false;
                }
                store.upsertProtocol(e.protocolIndex, std::move(row));
            } else if (pe.kind == ProtocolEventKind::RESULT) {
                if (isProgressPlaceholder(pe.result)) break;
                if (store.completedResultIds.insert(pe.result.id).second) {
                    ++store.resultCount;
                    store.pendingActionIds.erase(pe.result.id);
                    store.pendingOps = static_cast<int>(store.pendingActionIds.size());
                }
                if (rootAgent && rootAgent->hasSubAgent(row.actionName)) {
                    row.drillable = true;
                    row.actionType = "agent";
                    if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
                    if (Agent* sub = rootAgent->getSubAgent(row.actionName)) {
                        const std::string& finalOut = sub->responseOutput();
                        std::string summaryOnly = pe.result.summary;
                        bool placeholder = summaryOnly.empty() || summaryOnly == row.actionName ||
                                           summaryOnly == pe.result.toolName;
                        if (!finalOut.empty() && (row.body.empty() || placeholder)) {
                            if (protocol::looksLikeSymbolDump(finalOut)) {
                                row.body = "[symbol dump · " +
                                           std::to_string(finalOut.size()) +
                                           " bytes · collapsed]";
                            } else {
                                row.body = finalOut;
                            }
                            if (pe.result.elapsedMs > 0)
                                row.body +=
                                    "\n" + std::to_string(static_cast<int>(pe.result.elapsedMs)) + "ms";
                        }
                    }
                } else {
                    row.drillable = false;
                }
                store.upsertProtocol(e.protocolIndex, std::move(row));
            } else {
                store.upsertProtocol(e.protocolIndex, std::move(row));
            }
            if (atRoot) {
                // Selection pin / stick_bottom live-lock is owned by ShellModel
                // (followLiveEdgeIfLocked). Do not force selectedBlock here.
                (void)runningStickSelect;
                fx.needRebuild = true;
            } else {
                fx.needRefreshNested = true;
            }
            break;
        }
        case UiEventKind::TurnDone: {
            turn.done = true;
            turn.running = false;
            if (turn.turnStartMs > 0) {
                turn.lastTurnElapsedMs = nowMs() - turn.turnStartMs;
                turn.turnStartMs = 0;
            }
            const bool cancelled =
                e.text == "[cancelled]" || e.text == "cancelled" ||
                e.text.find("cancelled") != std::string::npos ||
                e.text.find("Operation was aborted") != std::string::npos ||
                e.text.find("ABORTED_BY_CALLBACK") != std::string::npos;
            if (cancelled) turn.failed = false;
            turn.status = cancelled ? "cancelled" : turn.failed ? "failed" : "done";
            turn.finalText = e.text;
            store.timelineState = e.text.empty() ? PageState::Empty : PageState::Populated;
            bool hasResponse = false;
            for (const auto& row : store.rootRows)
                if (row.kind == TimelineKind::Response || row.kind == TimelineKind::Final)
                    hasResponse = true;
            // Always close the chat with a visible terminal block — including
            // cancel — so a stop never looks like a silent hang. Prefer the
            // protocol RESPONSE when present; otherwise paint Final from TurnDone.
            if (!hasResponse && !e.text.empty()) {
                std::string body = e.text;
                if (cancelled && body != "[cancelled]" &&
                    body.find("cancelled") != std::string::npos)
                    body = "[cancelled by operator]";
                store.appendRoot(
                    {TimelineKind::Final, cancelled ? "cancelled" : "final", body,
                     !turn.failed || cancelled});
            }
            store.pendingActionIds.clear();
            store.pendingOps = 0;
            store.actionCount = 0;  // actN is per-turn, not sticky across done
            if (rootAgent) {
                for (auto& row : store.rootRows) {
                    if ((row.kind == TimelineKind::Action && row.actionType == "agent") ||
                        row.kind == TimelineKind::Result) {
                        if (rootAgent->hasSubAgent(row.actionName)) {
                            row.drillable = true;
                            if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
                        }
                    }
                }
            }
            fx.needPersist = true;
            if (atRoot)
                fx.needRebuild = true;
            else
                fx.needRefreshNested = true;
            break;
        }
        case UiEventKind::AskDialog:
            fx.hasAskDialog = true;
            fx.askDialog = e;
            break;
        case UiEventKind::AskDialogResult:
            fx.hasAskDialogResult = true;
            break;
    }
    return fx;
}

}  // namespace cortex::mk3::ui
