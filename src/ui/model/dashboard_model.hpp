#pragma once
// Dashboard navigation + inventory. Pure model; no rendering.
// Manifests hub is the primary registry surface (recursive manifests/).
// Section nav is a bottom pill — Ctrl-J/K cycles with short ease animation.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "src/core/agent_catalog.hpp"
#include "src/session/manager.hpp"

namespace cortex::mk3::ui::model {

enum class DashboardSection { Overview, Sessions, Manifests, Harness, Runtime, Help };

// Back-compat alias.
constexpr DashboardSection Agents = DashboardSection::Manifests;

enum class DashboardFocus { Navigation, Content };

struct DashboardState {
    DashboardSection section = DashboardSection::Overview;
    DashboardFocus focus = DashboardFocus::Content;  // default into content; pill is always visible
    int navigationIndex = 0;
    int sessionIndex = 0;
    int manifestIndex = 0;
    std::string manifestFilter;  // empty | agent | tool | feed | workflow | ...
    std::string tagFilter;       // empty or exact tag/category match
    std::string searchQuery;     // substring over name/summary/tags/relPath
    bool searchMode = false;     // / composing
    std::vector<session::SessionManager::SessionInfo> sessions;
    std::vector<catalog::ManifestEntry> manifests;
    std::vector<catalog::AgentEntry> agents;
    std::string notice;
    std::string manifestDir;

    // Pill cycle animation
    int navPrevIndex = 0;
    int64_t navAnimStartMs = 0;
    static constexpr int navAnimDurationMs = 220;
    static constexpr int sectionCount = 6;

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    float navAnimT() const {
        if (navAnimStartMs <= 0) return 1.f;
        float t = static_cast<float>(nowMs() - navAnimStartMs) /
                  static_cast<float>(navAnimDurationMs);
        if (t < 0.f) return 0.f;
        if (t > 1.f) return 1.f;
        return t;
    }

    bool navAnimating() const { return navAnimT() < 1.f; }

    void beginNavAnim(int fromIdx) {
        navPrevIndex = fromIdx;
        navAnimStartMs = nowMs();
    }

    void syncSection() { section = static_cast<DashboardSection>(navigationIndex); }

    void moveNavigation(int delta) {
        int from = navigationIndex;
        int next = navigationIndex + delta;
        // wrap
        if (next < 0) next = sectionCount - 1;
        if (next >= sectionCount) next = 0;
        if (next == navigationIndex) return;
        beginNavAnim(from);
        navigationIndex = next;
        syncSection();
        focus = DashboardFocus::Content;
    }

    void select(DashboardSection next) {
        int from = navigationIndex;
        int idx = static_cast<int>(next);
        if (idx != navigationIndex) beginNavAnim(from);
        section = next;
        navigationIndex = idx;
        focus = DashboardFocus::Content;
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

    void moveAgent(int delta) { moveManifest(delta); }

    void refreshSessions(const session::SessionManager& manager = session::SessionManager()) {
        sessions = manager.list();
        if (sessions.empty())
            sessionIndex = 0;
        else
            sessionIndex =
                std::max(0, std::min(static_cast<int>(sessions.size()) - 1, sessionIndex));
    }

    static bool containsFold(const std::string& hay, const std::string& needle) {
        if (needle.empty()) return true;
        auto lower = [](std::string s) {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        return lower(hay).find(lower(needle)) != std::string::npos;
    }

    void applyManifestFilters(std::vector<catalog::ManifestEntry> all) {
        manifests.clear();
        for (auto& m : all) {
            if (!manifestFilter.empty() && m.kind != manifestFilter) continue;
            if (!tagFilter.empty()) {
                bool hit = (m.category == tagFilter);
                for (const auto& t : m.tags)
                    if (t == tagFilter) {
                        hit = true;
                        break;
                    }
                if (!hit) continue;
            }
            if (!searchQuery.empty()) {
                bool hit = containsFold(m.name, searchQuery) || containsFold(m.summary, searchQuery) ||
                           containsFold(m.relPath, searchQuery) || containsFold(m.category, searchQuery);
                if (!hit) {
                    for (const auto& t : m.tags)
                        if (containsFold(t, searchQuery)) {
                            hit = true;
                            break;
                        }
                }
                if (!hit) continue;
            }
            manifests.push_back(std::move(m));
        }
        if (manifests.empty())
            manifestIndex = 0;
        else
            manifestIndex =
                std::max(0, std::min(static_cast<int>(manifests.size()) - 1, manifestIndex));
    }

    void refreshManifests() {
        auto all = catalog::discoverManifests(manifestDir);
        applyManifestFilters(std::move(all));
        agents = catalog::discoverAgents(manifestDir);
    }

    void refreshAgents() { refreshManifests(); }

    void refreshAll() {
        refreshSessions();
        refreshManifests();
    }

    void cycleManifestFilter() {
        static const char* kCycle[] = {"",         "agent",  "tool",   "feed",
                                       "workflow", "harness", "prompt", "skill"};
        int idx = 0;
        for (int i = 0; i < 8; ++i)
            if (manifestFilter == kCycle[i]) {
                idx = i;
                break;
            }
        manifestFilter = kCycle[(idx + 1) % 8];
        tagFilter.clear();
        refreshManifests();
    }

    // Cycle through popular tags present in the current unfiltered set.
    void cycleTagFilter() {
        auto all = catalog::discoverManifests(manifestDir);
        std::vector<std::string> tags;
        auto add = [&](const std::string& t) {
            if (t.empty()) return;
            if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
        };
        add("");  // all
        for (const auto& m : all) {
            add(m.category);
            for (const auto& t : m.tags) {
                if (t == m.kind) continue;  // kind already has f-filter
                add(t);
            }
        }
        // keep list short for QoL
        if (tags.size() > 24) tags.resize(24);
        int idx = 0;
        for (int i = 0; i < static_cast<int>(tags.size()); ++i)
            if (tags[static_cast<size_t>(i)] == tagFilter) {
                idx = i;
                break;
            }
        tagFilter = tags[static_cast<size_t>((idx + 1) % tags.size())];
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
        if (const auto* m = selectedManifest()) {
            if (m->kind == "agent" && m->launchable) {
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
