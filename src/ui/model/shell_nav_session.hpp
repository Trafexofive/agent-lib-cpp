#pragma once
// Nested drill + session timeline load/persist (F4b). Out-of-line ShellModel methods.
// Included at the bottom of inkcell_app_model.hpp after ShellModel is complete.

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace cortex::mk3::ui {

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

// Manual drill only (↳ Enter). Never auto-enter on AGENT action —
// the operator chooses when to open a sub-agent's full chat.
inline bool ShellModel::enterSubAgent(const std::string& name) {
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
    timelineFocus = false;
    composer.focused = true;
    transcriptView.stick_bottom = true;
    rebuildViews();
    return true;
}

inline bool ShellModel::enterSelected() {
    const TimelineRow* row = selectedRow();
    if (!row) return false;
    // Agent actions still drill into nested history (existing contract).
    if (row->drillable && !row->actionName.empty())
        return enterSubAgent(row->actionName);

    // Default click: open full-text reader for any body-bearing block.
    std::string body = row->body.empty() ? row->title : row->body;
    if (body.empty()) return false;

    std::string title;
    switch (row->kind) {
        case TimelineKind::User: title = "you"; break;
        case TimelineKind::Thought: title = "thought"; break;
        case TimelineKind::Response:
        case TimelineKind::Final: title = agentName.empty() ? "response" : agentName; break;
        case TimelineKind::Result:
            title = std::string(row->ok ? "result" : "result · error");
            if (!row->actionName.empty()) title += " · " + row->actionName;
            break;
        case TimelineKind::Action:
            title = row->actionType.empty() ? "action" : row->actionType;
            if (!row->actionName.empty()) title += " · " + row->actionName;
            break;
        case TimelineKind::Error: title = "error"; break;
        case TimelineKind::Status: title = "status"; break;
        case TimelineKind::Stream: title = "raw"; break;
        case TimelineKind::Log: title = row->title.empty() ? "notice" : row->title; break;
        default: title = row->title.empty() ? "block" : row->title; break;
    }
    chat::openBlockReader(blockReader, std::move(title), std::move(body));
    timelineFocus = true;
    composer.focused = false;
    return true;
}

inline bool ShellModel::goBack() {
    if (blockReader.open) {
        chat::closeBlockReader(blockReader);
        return true;
    }
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

inline void ShellModel::refreshNested() {
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

inline void ShellModel::reannotateDrillable() {
    // Session restore deserializes drillable flags from disk, but those can be
    // stale/false when the live Agent tree was rebuilt (or when older timelines
    // never set actionName). Re-resolve against the current rootAgent so ↳ Enter
    // works after resume the same way it does on first-start live turns.
    if (!rootAgent) return;
    auto fix = [this](TimelineRow& row) {
        // Recover agent name from titles when older/partial rows lost actionName.
        if (row.actionName.empty()) {
            // Live titles: "agent:reader  ↳ enter" / Result titles often include the name.
            auto recover = [](const std::string& s) -> std::string {
                // "agent:NAME" prefix
                if (s.rfind("agent:", 0) == 0) {
                    std::string n = s.substr(6);
                    auto sp = n.find_first_of(" \t");
                    if (sp != std::string::npos) n = n.substr(0, sp);
                    return n;
                }
                return {};
            };
            std::string n = recover(row.title);
            if (n.empty()) n = recover(row.actionType == "agent" ? row.title : "");
            if (!n.empty() && rootAgent->hasSubAgent(n)) {
                row.actionName = n;
                row.actionType = "agent";
            }
        }
        const bool isAgentish =
            row.actionType == "agent" ||
            (row.kind == TimelineKind::Action && !row.actionName.empty() &&
             rootAgent->hasSubAgent(row.actionName)) ||
            (row.kind == TimelineKind::Result && !row.actionName.empty() &&
             rootAgent->hasSubAgent(row.actionName));
        if (!isAgentish) {
            // Don't force-clear non-agent drillable (none today); only agent rows.
            if (row.actionType == "agent") row.drillable = false;
            return;
        }
        if (row.actionName.empty() || !rootAgent->hasSubAgent(row.actionName)) {
            row.drillable = false;
            return;
        }
        row.drillable = true;
        if (row.actionType.empty()) row.actionType = "agent";
        if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
    };
    for (auto& row : rootRows) fix(row);
    for (auto& row : nestedRows) fix(row);
}

inline void ShellModel::loadSessionRecords(const std::vector<SessionRecord>& records) {
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
    reannotateDrillable();
    timelineState = rootRows.empty() ? PageState::Empty : PageState::Populated;
    selectedBlock = 0;
    markProjFull();
    rebuildViews();
}

// Prefer structured ui_timeline when present (exact live parity).
// Fall back to records projection when older sessions lack the field.
inline void ShellModel::loadSessionUi(const Session& session) {
    activeProtocolRows.clear();
    pendingActionIds.clear();
    completedResultIds.clear();
    pendingOps = 0;
    if (!session.uiTimelineJson.empty()) {
        auto rows = deserializeTimeline(session.uiTimelineJson);
        if (!rows.empty()) {
            rootRows.assign(std::make_move_iterator(rows.begin()),
                            std::make_move_iterator(rows.end()));
            // Critical: re-bind ↳ drill targets to the LIVE agent tree.
            // Without this, resume shows agent rows but Enter is a no-op
            // (drillable false / empty actionName / subagent not resolved).
            reannotateDrillable();
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
inline void ShellModel::persistUiTimeline() {
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
    c.manifestPath = activeManifestPath;
    if (c.manifestPath.empty() && rootAgent &&
        !rootAgent->config().manifestPath.empty())
        c.manifestPath = rootAgent->config().manifestPath;
    c.generation = gen.fetch_add(1, std::memory_order_relaxed);
    session::AsyncUiTimelineWriter::instance().enqueue(std::move(c));
}

inline void ShellModel::persistUiTimelineFlush() {
    session::AsyncUiTimelineWriter::instance().flush();
}

inline void ShellModel::clearTranscript() {
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


}  // namespace cortex::mk3::ui
