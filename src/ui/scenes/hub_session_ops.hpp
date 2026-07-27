#pragma once
// Hub session/workflow actions — out-of-line MainScene methods.

#include <string>

namespace cortex::mk3::ui::scenes {

inline void MainScene::activate() {
    auto& dash = model_->dashboard;
    if (dash.section == model::DashboardSection::Sessions) {
        resumeSelectedSession();
        return;
    }
    if (dash.section == model::DashboardSection::Home) {
        model_->requestRoute(PendingRoute::Agent);
        return;
    }
    if (dash.section == model::DashboardSection::Manifests) {
        const auto* m = dash.selectedManifest();
        if (!m) {
            dash.flashNotice("no selection");
            return;
        }
        if (m->kind == "agent" && m->launchable) {
            // Real launch — REPL tick hot-swaps Agent then opens chat.
            dash.yankBuffer = "cortex-mk3 -m " + m->path + " --tui experimental";
            model_->pendingLaunchManifest = m->path;
            dash.flashNotice("launching " + m->name + "…");
            model_->launchError.clear();
            return;
        }
        if (m->kind == "workflow") {
            queueWorkflowRun(*m);
            return;
        }
        dash.flashNotice(m->kind + " · " + m->category + " · inspect only");
        return;
    }
}

inline bool MainScene::workflowSelectionActive() const {
    const auto* m = model_->dashboard.selectedManifest();
    return m && m->kind == "workflow" &&
           model::workflowRunnablePath(m->path, m->name);
}

// Infinite canvas stage: header + graph void + optional live strip.
inline void MainScene::queueWorkflowRun(const catalog::ManifestEntry& m) {
    auto& dash = model_->dashboard;
    if (!model::workflowRunnablePath(m.path, m.name)) {
        dash.flashNotice("workflow spec · not runnable");
        return;
    }
    if (model_->workflowRun.isLive()) {
        dash.flashNotice("workflow already running · Esc/x stop");
        return;
    }
    dash.yankBuffer = "workflow run " + m.path;
    model_->pendingRunWorkflow = m.path;
    dash.wfCanvasFocus = true;
    dash.flashNotice("running " + m.name + "…");
}

inline void MainScene::resumeLastWorkflow() {
    auto& dash = model_->dashboard;
    auto snap = model_->workflowRun.snapshot();
    std::string path = snap.path;
    if (path.empty()) {
        const auto* m = dash.selectedManifest();
        if (m && m->kind == "workflow") path = m->path;
    }
    if (path.empty()) {
        dash.flashNotice("no workflow to resume");
        return;
    }
    if (model_->workflowRun.isLive()) {
        dash.flashNotice("already running");
        return;
    }
    model_->pendingRunWorkflow = path;
    dash.wfCanvasFocus = true;
    dash.flashNotice("re-running " + (snap.name.empty() ? path : snap.name) + "…");
}

inline void MainScene::resumeSelectedSession() {
    if (!model_->rootAgent) {
        model_->dashboard.notice = "agent runtime unavailable";
        return;
    }
    session::SessionManager sessions;
    auto result = model::resumeDashboardSession(
        model_->dashboard, sessions,
        [&](const std::string& id) { model_->rootAgent->loadSession(id); });
    if (!result.ok) return;
    // loadSession may have repaired records:[] from the state
    // checkpoint into agent.history_ + re-saved the session file.
    // Prefer re-reading the session so the chat UI gets the same
    // rows the agent will use for the next turn.
    std::vector<SessionRecord> records = std::move(result.records);
    if (records.empty() && sessions.exists(result.sessionId)) {
        try {
            records = sessions.load(result.sessionId).records;
        } catch (...) {
        }
    }
    // Prefer full session (ui_timeline) so resume matches live chat.
    Session full;
    try {
        if (sessions.exists(result.sessionId))
            full = sessions.load(result.sessionId);
    } catch (...) {
    }
    if (full.records.empty() && !records.empty()) full.records = records;
    if (full.records.empty() && model_->rootAgent) {
        for (const auto& h : model_->rootAgent->history()) {
            SessionRecord rec;
            if (h.rfind("User: ", 0) == 0) {
                rec.role = SessionRecord::USER;
                rec.content = h.substr(6);
            } else if (h.rfind("Agent: ", 0) == 0) {
                rec.role = SessionRecord::AGENT;
                rec.content = h.substr(7);
            } else if (h.rfind("System: ", 0) == 0) {
                rec.role = SessionRecord::SYSTEM;
                rec.content = h.substr(8);
            } else {
                continue;
            }
            full.records.push_back(std::move(rec));
        }
    }
    model_->loadSessionUi(full);
    model_->activeSessionId = result.sessionId;
    model_->requestRoute(PendingRoute::Agent);
}

inline void MainScene::createSession() {
    if (!model_->rootAgent) {
        model_->dashboard.notice = "agent runtime unavailable";
        return;
    }
    session::SessionManager sessions;
    auto result = model::createDashboardSession(
        model_->dashboard, sessions, model_->rootAgent->name(),
        nonempty(model_->agentModel, cfg_.model),
        nonempty(model_->agentProvider, cfg_.provider),
        [&] { model_->rootAgent->clearHistory(); });
    if (!result.ok) return;
    model_->clearTranscript();
    model_->activeSessionId = result.sessionId;
    model_->requestRoute(PendingRoute::Agent);
}

inline void MainScene::killLiveSession() {
    // Operator-locked requirement: a way to end a live session in-place
    // from the Sessions page (not just refuse-to-delete). We:
    //   1) flip g_running so the worker exits its current iteration;
    //   2) explicitly tell the model the live run is cancelled;
    //   3) drop the persistence file (which the Sessions list can now
    //      re-read on next refresh — it's truly gone);
    //   4) clear the in-memory transcript so a stale chat footer
    //      doesn't haunt the operator.
    const auto* sel = model_->dashboard.selectedSession();
    if (!sel) {
        model_->dashboard.notice = "no session selected";
        return;
    }
    std::string id = sel->id;
    if (model_->activeSessionId.empty()) {
        // Selected the row but nothing live → just delete it.
        deleteSelectedSession();
        return;
    }
    if (model_->activeSessionId != id) {
        model_->dashboard.notice =
            "live is " + suffix(model_->activeSessionId) + " — select it to kill";
        return;
    }
    // 1. signal worker to stop. Next iteration check will exit before
    // a new prompt() round, and the bridge publishes TurnDone.
    g_running = false;
    // 2. clear live flags synchronously so the hub reflects the state
    // immediately (the worker may take a moment to publish TurnDone).
    model_->running = false;
    model_->status = "stopped";
    try {
        // Drain any coalesced ui_timeline write BEFORE remove — otherwise
        // AsyncUiTimelineWriter can resurrect the file after delete.
        session::AsyncUiTimelineWriter::instance().flush();
        session::SessionManager sessions;
        sessions.remove(id);
        // 3. clear in-memory session record of the dead session.
        model_->activeSessionId.clear();
        session::activeSession().clear();
        model_->pendingSubmit.clear();
        if (model_->rootAgent) model_->rootAgent->clearHistory();
        model_->clearTranscript();
        model_->dashboard.refreshSessions(sessions);
        model_->dashboard.notice = "killed live session " + suffix(id);
    } catch (const std::exception& e) {
        model_->dashboard.notice = std::string("kill failed: ") + e.what();
    }
}

inline void MainScene::forkSelectedSession() {
    const auto* sel = model_->dashboard.selectedSession();
    if (!sel) {
        model_->dashboard.notice = "no session selected";
        return;
    }
    try {
        session::SessionManager sessions;
        if (!sessions.exists(sel->id)) {
            model_->dashboard.notice = "session missing on disk";
            return;
        }
        std::string newId = session::mintSessionId();
        std::string title = sel->title.empty() ? ("fork of " + suffix(sel->id))
                                              : (sel->title + " (fork)");
        Session fork = session::forkSession(sessions, sel->id, newId, title);
        model_->dashboard.refreshSessions(sessions);
        // Select the new fork.
        for (int i = 0; i < static_cast<int>(model_->dashboard.sessions.size()); ++i) {
            if (model_->dashboard.sessions[static_cast<size_t>(i)].id == fork.id) {
                model_->dashboard.sessionIndex = i;
                break;
            }
        }
        model_->dashboard.notice =
            "forked " + suffix(sel->id) + " → " + suffix(fork.id) +
            (fork.uiTimelineJson.empty() ? " (records only)" : " (ui timeline)");
    } catch (const std::exception& e) {
        model_->dashboard.notice = std::string("fork failed: ") + e.what();
    }
}

inline void MainScene::retitleSelectedSession() {
    const auto* sel = model_->dashboard.selectedSession();
    if (!sel) {
        model_->dashboard.notice = "no session selected";
        return;
    }
    try {
        session::SessionManager sessions;
        Session s = sessions.load(sel->id);
        if (s.id.empty()) {
            model_->dashboard.notice = "session missing on disk";
            return;
        }
        // Prefer first user record as title; if already titled, clear (toggle).
        std::string next;
        if (sel->title.empty() || sel->title == suffix(sel->id)) {
            for (const auto& rec : s.records) {
                if (rec.role == SessionRecord::USER && !rec.content.empty()) {
                    next = rec.content;
                    auto nl = next.find('\n');
                    if (nl != std::string::npos) next = next.substr(0, nl);
                    if (next.size() > 48) next = next.substr(0, 45) + "...";
                    break;
                }
            }
            if (next.empty()) next = "session " + suffix(sel->id);
        } else {
            next.clear();  // clear custom title → fall back to prompt/id
        }
        if (!session::setSessionTitle(sessions, sel->id, next)) {
            model_->dashboard.notice = "title update failed";
            return;
        }
        model_->dashboard.refreshSessions(sessions);
        model_->dashboard.notice =
            next.empty() ? ("cleared title on " + suffix(sel->id))
                         : ("titled " + suffix(sel->id) + " → " + next);
    } catch (const std::exception& e) {
        model_->dashboard.notice = std::string("title failed: ") + e.what();
    }
}

inline void MainScene::exportSelectedSession() {
    // Vet-fix smarter export: portable .json via SessionManager::exportToFile
    // rather than dumping raw. Path is well-known: /tmp/mk3-session-<id>.json.
    // Falls back to a notice if anything stalls so the operator can recover.
    const auto* sel = model_->dashboard.selectedSession();
    if (!sel) {
        model_->dashboard.notice = "no session selected";
        return;
    }
    try {
        session::SessionManager sessions;
        std::string path = "/tmp/mk3-session-" + sel->id + ".json";
        if (sessions.exportToFile(sel->id, path)) {
            model_->dashboard.notice = "exported → " + path;
        } else {
            model_->dashboard.notice = "export failed (empty/loadable?): " + sel->id;
        }
    } catch (const std::exception& e) {
        model_->dashboard.notice = std::string("export error: ") + e.what();
    }
}

inline void MainScene::deleteSelectedSession() {
    const auto* sel = model_->dashboard.selectedSession();
    if (!sel) {
        model_->dashboard.notice = "no session selected";
        return;
    }
    std::string id = sel->id;
    // Vet-fix: refuse to delete the active session — that would
    // silently strand the live agent. Operator must end/clear first.
    if (model_->activeSessionId == id) {
        model_->dashboard.notice =
            "active session — /clear or resume another first (esc to clear)";
        return;
    }
    try {
        session::SessionManager sessions;
        sessions.remove(id);
        // Clamp selector if we just shrank the list.
        model_->dashboard.sessionIndex =
            std::max(0, std::min(model_->dashboard.sessionIndex,
                                 static_cast<int>(model_->dashboard.sessions.size()) - 1));
        model_->dashboard.refreshSessions(sessions);
        model_->dashboard.notice = "deleted " + id;
    } catch (const std::exception& e) {
        model_->dashboard.notice = std::string("delete failed: ") + e.what();
    }
}


}  // namespace cortex::mk3::ui::scenes
