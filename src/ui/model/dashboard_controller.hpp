#pragma once
// Dashboard session lifecycle controller. Testable without rendering or Agent construction.

#include <chrono>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "src/core/types.hpp"
#include "src/session/controller.hpp"
#include "src/session/manager.hpp"
#include "src/ui/model/dashboard_model.hpp"

namespace cortex::mk3::ui::model {

struct DashboardSessionResult {
    bool ok = false;
    std::string sessionId;
    std::string notice;
    std::vector<SessionRecord> records;
};

inline DashboardSessionResult resumeDashboardSession(
    DashboardState& dashboard, session::SessionManager& sessions,
    const std::function<void(const std::string&)>& loadAgentSession) {
    const auto* selected = dashboard.selectedSession();
    if (!selected) return {false, {}, "no session selected", {}};
    try {
        Session loaded = sessions.load(selected->id);
        if (loaded.id.empty()) return {false, {}, "session not found", {}};
        loadAgentSession(selected->id);
        dashboard.notice = "resumed " + selected->id;
        return {true, selected->id, dashboard.notice, std::move(loaded.records)};
    } catch (const std::exception& error) {
        dashboard.notice = std::string("resume failed: ") + error.what();
        return {false, {}, dashboard.notice, {}};
    }
}

inline DashboardSessionResult createDashboardSession(
    DashboardState& dashboard, session::SessionManager& sessions,
    const std::string& agentName, const std::string& model, const std::string& provider,
    const std::function<void()>& clearAgentSession,
    const std::string& explicitId = {}) {
    std::string id = explicitId;
    if (id.empty()) {
        // Unified mint (F6) — same scheme as CLI / lazy arm / hub fork.
        id = session::mintSessionId();
    }
    try {
        // Vet-fix: do NOT call sessions.create(). The file is created only
        // once saveSession() decides the turn had real content (and only
        // then does the id appear in the Sessions page listing). This
        // removes the empty-zero-record file litter the hub accumulated.
        // Caller is expected to set agent.sessionId = id via clearAgentSession
        // callback so resumes work.
        (void)sessions; // silence unused warning when stub
        clearAgentSession();
        dashboard.refreshSessions(sessions);
        dashboard.notice = "armed " + id;
        return {true, id, dashboard.notice, {}};
    } catch (const std::exception& error) {
        dashboard.notice = std::string("create failed: ") + error.what();
        return {false, {}, dashboard.notice, {}};
    }
}

}  // namespace cortex::mk3::ui::model
