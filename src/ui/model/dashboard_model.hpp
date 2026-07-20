#pragma once
// Dashboard navigation/session/agent inventory. Pure model; no rendering.

#include <algorithm>
#include <string>
#include <vector>

#include "src/core/agent_catalog.hpp"
#include "src/session/manager.hpp"

namespace cortex::mk3::ui::model {

enum class DashboardSection { Overview, Sessions, Agents, Harness, Runtime, Help };

enum class DashboardFocus { Navigation, Content };

struct DashboardState {
    DashboardSection section = DashboardSection::Overview;
    DashboardFocus focus = DashboardFocus::Navigation;
    int navigationIndex = 0;
    int sessionIndex = 0;
    int agentIndex = 0;
    std::vector<session::SessionManager::SessionInfo> sessions;
    std::vector<catalog::AgentEntry> agents;
    std::string notice;
    std::string manifestDir;  // optional override for catalog discovery

    static constexpr int sectionCount = 6;

    void syncSection() { section = static_cast<DashboardSection>(navigationIndex); }

    void moveNavigation(int delta) {
        navigationIndex = std::max(0, std::min(sectionCount - 1, navigationIndex + delta));
        syncSection();
    }

    void moveSession(int delta) {
        if (sessions.empty()) {
            sessionIndex = 0;
            return;
        }
        sessionIndex = std::max(0, std::min(static_cast<int>(sessions.size()) - 1, sessionIndex + delta));
    }

    void moveAgent(int delta) {
        if (agents.empty()) {
            agentIndex = 0;
            return;
        }
        agentIndex = std::max(0, std::min(static_cast<int>(agents.size()) - 1, agentIndex + delta));
    }

    void select(DashboardSection next) {
        section = next;
        navigationIndex = static_cast<int>(next);
    }

    void refreshSessions(const session::SessionManager& manager = session::SessionManager()) {
        sessions = manager.list();
        if (sessions.empty()) sessionIndex = 0;
        else sessionIndex = std::max(0, std::min(static_cast<int>(sessions.size()) - 1, sessionIndex));
    }

    void refreshAgents() {
        agents = catalog::discoverAgents(manifestDir);
        if (agents.empty()) agentIndex = 0;
        else agentIndex = std::max(0, std::min(static_cast<int>(agents.size()) - 1, agentIndex));
    }

    void refreshAll() {
        refreshSessions();
        refreshAgents();
    }

    const session::SessionManager::SessionInfo* selectedSession() const {
        if (sessionIndex < 0 || sessionIndex >= static_cast<int>(sessions.size())) return nullptr;
        return &sessions[static_cast<size_t>(sessionIndex)];
    }

    const catalog::AgentEntry* selectedAgent() const {
        if (agentIndex < 0 || agentIndex >= static_cast<int>(agents.size())) return nullptr;
        return &agents[static_cast<size_t>(agentIndex)];
    }
};

inline const char* dashboardSectionName(DashboardSection section) {
    switch (section) {
        case DashboardSection::Overview: return "Overview";
        case DashboardSection::Sessions: return "Sessions";
        case DashboardSection::Agents: return "Agents";
        case DashboardSection::Harness: return "Harness";
        case DashboardSection::Runtime: return "Runtime";
        case DashboardSection::Help: return "Help";
    }
    return "Overview";
}

}  // namespace cortex::mk3::ui::model
