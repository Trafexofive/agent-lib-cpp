#pragma once
// Dashboard navigation + inventory. Pure model; no rendering.
// Manifests hub is the primary registry surface (recursive manifests/).

#include <algorithm>
#include <string>
#include <vector>

#include "src/core/agent_catalog.hpp"
#include "src/session/manager.hpp"

namespace cortex::mk3::ui::model {

enum class DashboardSection { Overview, Sessions, Manifests, Harness, Runtime, Help };

// Back-compat alias — older call sites used Agents.
constexpr DashboardSection Agents = DashboardSection::Manifests;

enum class DashboardFocus { Navigation, Content };

struct DashboardState {
    DashboardSection section = DashboardSection::Overview;
    DashboardFocus focus = DashboardFocus::Navigation;
    int navigationIndex = 0;
    int sessionIndex = 0;
    int manifestIndex = 0;
    std::string manifestFilter;  // empty | agent | tool | feed | workflow | ...
    std::vector<session::SessionManager::SessionInfo> sessions;
    std::vector<catalog::ManifestEntry> manifests;
    std::vector<catalog::AgentEntry> agents;  // launchable agents only (top-level)
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
        sessionIndex =
            std::max(0, std::min(static_cast<int>(sessions.size()) - 1, sessionIndex + delta));
    }

    void moveManifest(int delta) {
        if (manifests.empty()) {
            manifestIndex = 0;
            return;
        }
        manifestIndex =
            std::max(0, std::min(static_cast<int>(manifests.size()) - 1, manifestIndex + delta));
    }

    // Legacy name used by main_scene during transition.
    void moveAgent(int delta) { moveManifest(delta); }

    void select(DashboardSection next) {
        section = next;
        navigationIndex = static_cast<int>(next);
    }

    void refreshSessions(const session::SessionManager& manager = session::SessionManager()) {
        sessions = manager.list();
        if (sessions.empty())
            sessionIndex = 0;
        else
            sessionIndex =
                std::max(0, std::min(static_cast<int>(sessions.size()) - 1, sessionIndex));
    }

    void refreshManifests() {
        auto all = catalog::discoverManifests(manifestDir);
        if (!manifestFilter.empty()) {
            std::vector<catalog::ManifestEntry> filtered;
            for (auto& m : all)
                if (m.kind == manifestFilter) filtered.push_back(std::move(m));
            manifests = std::move(filtered);
        } else {
            manifests = std::move(all);
        }
        if (manifests.empty())
            manifestIndex = 0;
        else
            manifestIndex =
                std::max(0, std::min(static_cast<int>(manifests.size()) - 1, manifestIndex));

        // Launchable agents (top-level name resolution still uses discoverAgents).
        agents = catalog::discoverAgents(manifestDir);
    }

    void refreshAgents() { refreshManifests(); }

    void refreshAll() {
        refreshSessions();
        refreshManifests();
    }

    void cycleManifestFilter() {
        // empty → agent → tool → feed → workflow → harness → prompt → empty
        static const char* kCycle[] = {"",     "agent",  "tool",   "feed",
                                       "workflow", "harness", "prompt", "skill"};
        int idx = 0;
        for (int i = 0; i < 8; ++i)
            if (manifestFilter == kCycle[i]) {
                idx = i;
                break;
            }
        manifestFilter = kCycle[(idx + 1) % 8];
        refreshManifests();
    }

    const session::SessionManager::SessionInfo* selectedSession() const {
        if (sessionIndex < 0 || sessionIndex >= static_cast<int>(sessions.size())) return nullptr;
        return &sessions[static_cast<size_t>(sessionIndex)];
    }

    const catalog::ManifestEntry* selectedManifest() const {
        if (manifestIndex < 0 || manifestIndex >= static_cast<int>(manifests.size()))
            return nullptr;
        return &manifests[static_cast<size_t>(manifestIndex)];
    }

    const catalog::AgentEntry* selectedAgent() const {
        // Prefer selected launchable manifest as agent view.
        if (const auto* m = selectedManifest()) {
            if (m->kind == "agent" && m->launchable) {
                // Find matching AgentEntry by path if present.
                for (const auto& a : agents)
                    if (a.manifestPath == m->path) return &a;
            }
        }
        return nullptr;
    }
};

inline const char* dashboardSectionName(DashboardSection section) {
    switch (section) {
        case DashboardSection::Overview: return "Overview";
        case DashboardSection::Sessions: return "Sessions";
        case DashboardSection::Manifests: return "Manifests";
        case DashboardSection::Harness: return "Harness";
        case DashboardSection::Runtime: return "Runtime";
        case DashboardSection::Help: return "Help";
    }
    return "Overview";
}

}  // namespace cortex::mk3::ui::model
