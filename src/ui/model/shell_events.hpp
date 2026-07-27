#pragma once
// UiEvent apply/drain product side-effects (F7). Out-of-line ShellModel methods.

#include <string>

namespace cortex::mk3::ui {

inline bool ShellModel::isProgressPlaceholder(const ProtocolResult& result) {
    return cortex::mk3::ui::isProgressPlaceholder(result);
}

inline void ShellModel::upsertProtocolRow(size_t index, TimelineRow row) {
    upsertProtocol(index, std::move(row));
}

// F3: pure reduction in reduceUiEvent; this method handles product side-effects.
inline void ShellModel::apply(const UiEvent& e) {
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
        // Transient pops auto-dismiss; retries still collapse by id while live.
        n.lifetimeMs = chat::kDefaultToastLifetimeMs;
        n.detail = "esc dismiss";
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
inline void ShellModel::settleAsk(AgentBridge& bridge) {
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

inline void ShellModel::drain(AgentBridge& bridge) {
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


}  // namespace cortex::mk3::ui
