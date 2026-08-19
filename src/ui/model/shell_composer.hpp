#pragma once
// Composer submit + prompt history (F7). Out-of-line ShellModel methods.

#include <string>
#include <vector>

namespace cortex::mk3::ui {

inline void ShellModel::appendNotice(const std::string& title, const std::vector<std::string>& lines) {
    TimelineRow row;
    row.kind = TimelineKind::Log;
    row.title = title;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) row.body += "\n";
        row.body += lines[i];
    }
    pushRow(std::move(row));
}

inline bool ShellModel::historyPrevious() {
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

inline bool ShellModel::historyNext() {
    if (promptHistory.empty() || running || !atRoot()) return false;
    if (promptHistoryIndex >= static_cast<int>(promptHistory.size())) return false;
    ++promptHistoryIndex;
    composer.value = promptHistoryIndex == static_cast<int>(promptHistory.size())
                         ? promptHistoryDraft
                         : promptHistory[static_cast<size_t>(promptHistoryIndex)];
    composer.cursor = static_cast<int>(composer.value.size());
    return true;
}

inline bool ShellModel::submitComposer() {
    if (!atRoot()) {
        chat::Notification n;
        n.id = "submit-blocked";
        n.source = "composer";
        n.severity = "warn";
        n.title = "drill out first · esc";
        n.lifetimeMs = 2500;
        notificationStack.push(std::move(n));
        return false;
    }
    std::string text = composer.value;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r'))
        text.pop_back();
    if (text.empty()) return false;

    // Live turn → steer buffer (no warn). Injected at next iteration boundary.
    if (running) {
        if (rootAgent) rootAgent->queueSteer(text);
        else {
            // No agent ptr — hold for next pendingSubmit when turn ends.
            if (!pendingSteerBuffer.empty()) pendingSteerBuffer += "\n\n";
            pendingSteerBuffer += text;
        }
        pushRow({TimelineKind::Status, "steer", "⟹ " + text, true});
        composer.value.clear();
        composer.cursor = 0;
        composer.scroll_row = 0;
        rebuildViews();
        chat::Notification n;
        n.id = "steer";
        n.source = "steer";
        n.severity = "info";
        n.title = "steered · injects next step";
        n.lifetimeMs = 2200;
        notificationStack.push(std::move(n));
        return true;
    }
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
    pushPromptHistory(text);
    pushRow({TimelineKind::User, "you", text, true});
    composer.value.clear();
    composer.cursor = 0;
    composer.scroll_row = 0;
    rebuildViews();
    return true;
}

}  // namespace cortex::mk3::ui
