#pragma once
// Hub session/workflow/tool/relic actions — out-of-line MainScene methods.

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <json/json.h>

#include "src/core/agent_catalog.hpp"
#include "src/providers/factory.hpp"
#include "src/tools/dispatch.hpp"

namespace cortex::mk3::ui::scenes {

// Smoke-test defaults for the registered builtin tools. We don't try to be
// clever — just enough params that each builtin can execute and return a
// real result. Script-based tools (registered via tool.yml at agent boot)
// aren't covered here; they'd need their manifest loaded.
inline Json::Value defaultParamsForTool(const std::string& name) {
    Json::Value p(Json::objectValue);
    if (name == "exec") p["command"] = "echo hub-tool-smoke";
    else if (name == "list") p["path"] = ".";
    else if (name == "grep") { p["pattern"] = "README"; p["path"] = "."; }
    else if (name == "fs_read") p["path"] = "README.md";
    else if (name == "fs_write") {
        p["path"] = "/tmp/mk3-hub-tool-smoke.txt";
        p["content"] = "hub smoke test";
    }
    else if (name == "json") p["data"] = std::string("{\"k\":\"v\"}");
    else if (name == "web_fetch") p["url"] = "http://example.com/";
    else if (name == "sleep") p["ms"] = 1;
    else if (name == "artifact") {
        p["name"] = "hub-smoke";
        p["content"] = std::string("smoke");
    }
    else if (name == "ask_tool") {
        p["title"] = "smoke test";
        Json::Value card;
        card["id"] = "response";
        card["type"] = "text";
        card["title"] = "smoke";
        card["defaultValue"] = "ok";
        p["cards"] = Json::Value(Json::arrayValue);
        p["cards"].append(card);
    }
    return p;
}

// Parse endpoints from a relic.yml — minimal line scanner, no full YAML.
// Looks for `  - name: <endpoint>` lines; everything else is ignored.
inline std::vector<std::string> parseRelicEndpoints(const std::string& path) {
    std::vector<std::string> eps;
    std::ifstream f(path);
    if (!f) return eps;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string yaml = ss.str();
    std::istringstream iss(yaml);
    std::string line;
    while (std::getline(iss, line)) {
        auto pos = line.find("- name:");
        if (pos == std::string::npos) continue;
        std::string ep = line.substr(pos + 7);
        size_t a = ep.find_first_not_of(" \t");
        size_t b = ep.find_last_not_of(" \t\r\n");
        if (a == std::string::npos || b == std::string::npos) continue;
        eps.push_back(ep.substr(a, b - a + 1));
    }
    return eps;
}

inline void MainScene::activate() {
    auto& dash = model_->dashboard;
    if (dash.section == model::DashboardSection::Sessions) {
        resumeSelectedSession();
        return;
    }
    if (dash.section == model::DashboardSection::Home) {
        const int c = dash.homeCursor;
        if (c == 1) {
            createSession();
            return;
        }
        if (c == 2) {
            dash.select(model::DashboardSection::Sessions);
            return;
        }
        if (c == 3) {
            dash.select(model::DashboardSection::Manifests);
            return;
        }
        if (c >= model::DashboardState::kHomeActionN) {
            int r = c - model::DashboardState::kHomeActionN;
            const std::string& liveId = model_->activeSessionId;
            std::vector<int> recentIdx;
            if (!liveId.empty()) {
                for (int i = 0; i < static_cast<int>(dash.sessions.size()); ++i)
                    if (dash.sessions[static_cast<size_t>(i)].id == liveId) {
                        recentIdx.push_back(i);
                        break;
                    }
            }
            for (int i = 0; i < static_cast<int>(dash.sessions.size()) &&
                             static_cast<int>(recentIdx.size()) < 6; ++i) {
                if (!liveId.empty() && dash.sessions[static_cast<size_t>(i)].id == liveId)
                    continue;
                recentIdx.push_back(i);
            }
            if (r >= 0 && r < static_cast<int>(recentIdx.size())) {
                dash.sessionIndex = recentIdx[static_cast<size_t>(r)];
                resumeSelectedSession();
                return;
            }
        }
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
            // Full workflow page (canvas + run), mirroring the tool page.
            model_->activeWorkflowManifestPath = m->path;
            model_->activeWorkflowName = m->name;
            model_->requestRoute(PendingRoute::Workflow);
            dash.flashNotice("workflow · " + m->name);
            return;
        }
        if (m->kind == "tool") {
            model_->activeToolManifestPath = m->path;
            model_->activeToolName = m->name;
            model_->requestRoute(PendingRoute::Tool);
            dash.flashNotice("tool · " + m->name);
            return;
        }
        if (m->kind == "relic") {
            model_->activeRelicManifestPath = m->path;
            model_->activeRelicName = m->name;
            // Seed endpoint inventory for the page + card cache.
            queueRelicRun(*m);
            model_->requestRoute(PendingRoute::Relic);
            dash.flashNotice("relic · " + m->name);
            return;
        }
        dash.flashNotice(m->kind + " · " + m->category + " · inspect only");
        return;
    }
    if (dash.section == model::DashboardSection::Tools) {
        const auto* m = dash.selectedManifest();
        if (m && m->kind == "tool") {
            model_->activeToolManifestPath = m->path;
            model_->activeToolName = m->name;
            model_->requestRoute(PendingRoute::Tool);
            dash.flashNotice("tool · " + m->name);
            return;
        }
        dash.flashNotice("no tool selected");
        return;
    }
    if (dash.section == model::DashboardSection::Relics) {
        const auto* m = dash.selectedManifest();
        if (m && m->kind == "relic") {
            model_->activeRelicManifestPath = m->path;
            model_->activeRelicName = m->name;
            queueRelicRun(*m);
            model_->requestRoute(PendingRoute::Relic);
            dash.flashNotice("relic · " + m->name);
            return;
        }
        dash.flashNotice("no relic selected");
        return;
    }
    if (dash.section == model::DashboardSection::Workflows) {
        // Enter opens the full workflow page (run stays inside the page via ↵/r).
        auto all = catalog::discoverManifests(dash.manifestDir);
        std::vector<catalog::ManifestEntry> wfs;
        for (const auto& e : all)
            if (e.kind == "workflow") wfs.push_back(e);
        if (wfs.empty())
            for (const auto& m : dash.manifests)
                if (m.kind == "workflow") wfs.push_back(m);
        if (wfs.empty()) {
            dash.flashNotice("no workflow selected");
            return;
        }
        const auto* cur = dash.selectedManifest();
        int sel = 0;
        for (int i = 0; i < (int)wfs.size(); ++i)
            if (cur && wfs[(size_t)i].path == cur->path) {
                sel = i;
                break;
            }
        sel = std::max(0, std::min(sel, (int)wfs.size() - 1));
        const auto& m = wfs[(size_t)sel];
        model_->activeWorkflowManifestPath = m.path;
        model_->activeWorkflowName = m.name;
        model_->requestRoute(PendingRoute::Workflow);
        dash.flashNotice("workflow · " + m.name);
        return;
    }
}

// Run a tool with smoke-test defaults; record outcome in dashboard state.
// Synchronous (tools fire-and-forget; no need for a worker thread for
// quick builtins). For long-running tools the caller can re-press Enter.
inline void MainScene::queueToolRun(const catalog::ManifestEntry& m) {
    auto& dash = model_->dashboard;
    const std::string name = m.name;
    Json::Value params = defaultParamsForTool(name);
    auto t0 = std::chrono::steady_clock::now();
    std::string raw;
    try {
        tools::registerDefaults();
        raw = tools::dispatch(name, params);
    } catch (const std::exception& e) {
        raw = std::string("{\"success\":false,\"error\":\"") + e.what() + "\"}";
    }
    auto t1 = std::chrono::steady_clock::now();
    int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    auto& r = dash.toolRun;
    r.toolName = name;
    r.elapsedMs = elapsed;
    Json::Value parsed;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream ss(raw);
    if (Json::parseFromStream(rb, ss, &parsed, &errs) && parsed.isObject()) {
        r.success = parsed.get("success", false).asBool();
        r.output = parsed.get("output", "").asString();
        r.error = parsed.get("error", "").asString();
        if (r.output.empty()) r.output = raw;  // surface non-output JSON
    } else {
        r.success = false;
        r.output = raw;
        r.error = "non-json response";
    }
    dash.flashNotice(name + (r.success ? " ok" : " fail") + " · " +
                     std::to_string(elapsed) + "ms");
}

// List endpoints from relic.yml; mark relic as healthy when endpoints parse.
// Actual endpoint invocation (HTTP/managed Docker) lands in a follow-up
// slice — for now the detail panel shows the endpoint inventory so the
// operator can see what the relic exposes.
inline void MainScene::queueRelicRun(const catalog::ManifestEntry& m) {
    auto& dash = model_->dashboard;
    auto& r = dash.relicRun;
    r.relicName = m.name;
    r.endpoints = parseRelicEndpoints(m.path);
    r.endpoint = r.endpoints.empty() ? std::string() : r.endpoints.front();
    r.healthy = !r.endpoints.empty();
    r.output = std::to_string(r.endpoints.size()) + " endpoint(s) parsed";
    dash.flashNotice(m.name + (r.healthy ? " · " + std::to_string(r.endpoints.size()) + " endpoints"
                                         : " · no endpoints"));
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
    // Live turn owns the agent slot. Switching sessions mid-turn races history_
    // and kills the point of "live". Same session → just return to chat.
    {
        const auto* sel = model_->dashboard.selectedSession();
        if (model_->running && sel) {
            if (!model_->activeSessionId.empty() && sel->id == model_->activeSessionId) {
                model_->dashboard.flashNotice("live · returning to chat");
                model_->requestRoute(PendingRoute::Agent);
                return;
            }
            model_->dashboard.flashNotice(
                "live turn on " + suffix(model_->activeSessionId) +
                " — stop (x) before resuming another session");
            return;
        }
    }
    // Apply session CWD on resume — operator can change CWD setting, hit
    // resume, and tools in the resumed session inherit the new process CWD.
    std::string cwd = applySessionCwd();
    if (!model_->sessionCwd.empty()) {
        if (cwd.empty()) {
            model_->dashboard.flashNotice(
                "cwd · invalid path, resumed in process CWD");
        } else {
            model_->dashboard.flashNotice("cwd · " + cwd);
        }
    }
    session::SessionManager sessions;
    auto result = model::resumeDashboardSession(
        model_->dashboard, sessions,
        [&](const std::string& id) {
            model_->rootAgent->loadSession(id);
            // Subagent history lives in the state checkpoint, not session.records.
            // Without this, ↳ enter after hub resume shows empty nested chats.
            model_->rootAgent->loadStateCheckpoint(id);
        });
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
    // Prefer session identity (agent/provider/model). If the session points at a
    // different manifest than the live agent, hot-swap so hub resume matches -c.
    {
        std::string wantManifest;
        auto it = full.metadata.find("manifest_path");
        if (it != full.metadata.end()) wantManifest = it->second;
        if (wantManifest.empty() && !isPlaceholderAgentName(full.agentName)) {
            std::string err;
            wantManifest = catalog::resolveAgent(full.agentName, "", &err);
            if (wantManifest.empty()) {
                // config/agents (hub launchable) is not always on catalog roots.
                namespace fs = std::filesystem;
                fs::path cfg = fs::current_path() / "config" / "agents" /
                               full.agentName / "agent.yml";
                std::error_code ec;
                if (fs::is_regular_file(cfg, ec))
                    wantManifest = fs::absolute(cfg).string();
            }
        }
        const bool livePlaceholder =
            isPlaceholderAgentName(model_->agentName) ||
            model_->activeManifestPath.empty();
        const bool identityMismatch =
            (!wantManifest.empty() && wantManifest != model_->activeManifestPath) ||
            (!isPlaceholderAgentName(full.agentName) &&
             full.agentName != model_->agentName);
        if (identityMismatch || (livePlaceholder && !wantManifest.empty())) {
            // Full rebuild via repl tick so tools/prompts match the session agent.
            if (wantManifest.empty()) {
                model_->dashboard.flashNotice(
                    "resume failed — no manifest for agent " + full.agentName);
                return;
            }
            if (model_->running) {
                model_->dashboard.flashNotice(
                    "live turn — stop before resume onto another agent");
                return;
            }
            model_->pendingResumeSessionId = result.sessionId;
            model_->pendingLaunchManifest = wantManifest;
            model_->agentName = full.agentName.empty() ? model_->agentName : full.agentName;
            model_->agentProvider = full.provider;
            model_->agentModel = full.model;
            model_->dashboard.flashNotice("resuming · " + full.agentName);
            return;  // repl builds agent then loads session
        }
        if (!full.agentName.empty()) model_->agentName = full.agentName;
        if (!full.model.empty()) model_->agentModel = full.model;
        if (!full.provider.empty()) model_->agentProvider = full.provider;
        // Same-manifest resume: apply session engine onto the live agent.
        if (model_->rootAgent) {
            const auto& c = model_->rootAgent->config();
            if (model_->agentName.empty() && !c.name.empty()) model_->agentName = c.name;
            if (model_->agentModel.empty() && !c.model.empty()) model_->agentModel = c.model;
            if (model_->agentProvider.empty() && !c.provider.empty())
                model_->agentProvider = c.provider;
            if (!full.provider.empty() && !full.model.empty() &&
                (c.provider != full.provider || c.model != full.model)) {
                auto p = providers::createProvider(full.provider, full.model);
                if (p) {
                    p->setQuietLogs(true);
                    model_->rootAgent->setProvider(p, full.provider, full.model);
                }
            }
        }
    }
    model_->reannotateDrillable();
    model_->activeSessionId = result.sessionId;
    model_->requestRoute(PendingRoute::Agent);
}

inline void MainScene::createSession() {
    if (!model_->rootAgent) {
        model_->dashboard.notice = "agent runtime unavailable";
        return;
    }
    // Apply session CWD before the session is born — every tool it spawns
    // (exec, fs_*, etc.) inherits the new process CWD.
    std::string cwd = applySessionCwd();
    if (!model_->sessionCwd.empty()) {
        if (cwd.empty()) {
            model_->dashboard.flashNotice(
                "cwd · invalid path, session created in " + std::string("process CWD"));
        } else {
            model_->dashboard.flashNotice("cwd · " + cwd);
        }
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
    if (model_->rootAgent)
        model_->rootAgent->requestStop(RunStopKind::Operator);
    else
        requestRunStop(RunStopKind::Operator);
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
