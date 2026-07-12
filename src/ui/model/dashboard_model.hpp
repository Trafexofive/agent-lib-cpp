#pragma once
// Dashboard navigation/session inventory. Pure model; no rendering or Agent logic.

#include <algorithm>
#include <string>
#include <vector>

#include "src/session/manager.hpp"

namespace cortex::mk3::ui::model {

enum class DashboardSection { Overview, Sessions, Harness, Runtime, Help };

enum class DashboardFocus { Navigation, Content };

struct DashboardState {
    DashboardSection section = DashboardSection::Overview;
    DashboardFocus focus = DashboardFocus::Navigation;
    int navigationIndex = 0;
    int sessionIndex = 0;
    std::vector<session::SessionManager::SessionInfo> sessions;
    std::string notice;

    static constexpr int sectionCount = 5;

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

    void select(DashboardSection next) {
        section = next;
        navigationIndex = static_cast<int>(next);
    }

    void refreshSessions(const session::SessionManager& manager = session::SessionManager()) {
        sessions = manager.list();
        if (sessions.empty()) sessionIndex = 0;
        else sessionIndex = std::max(0, std::min(static_cast<int>(sessions.size()) - 1, sessionIndex));
    }

    const session::SessionManager::SessionInfo* selectedSession() const {
        if (sessionIndex < 0 || sessionIndex >= static_cast<int>(sessions.size())) return nullptr;
        return &sessions[static_cast<size_t>(sessionIndex)];
    }
};

inline const char* dashboardSectionName(DashboardSection section) {
    switch (section) {
        case DashboardSection::Overview: return "Overview";
        case DashboardSection::Sessions: return "Sessions";
        case DashboardSection::Harness: return "Harness";
        case DashboardSection::Runtime: return "Runtime";
        case DashboardSection::Help: return "Help";
    }
    return "Overview";
}

}  // namespace cortex::mk3::ui::model
