#pragma once
// Dashboard model — tight IA, operable facets, dock focus.
// Sections (pill): Home · Sessions · Manifests · Help
// Harness/Runtime live on Home (not peer tabs).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "src/core/agent_catalog.hpp"
#include "src/session/manager.hpp"

namespace cortex::mk3::ui::model {

enum class DashboardSection { Home = 0, Sessions = 1, Manifests = 2, Help = 3 };

// Back-compat names used by older call sites / tests.
constexpr DashboardSection Overview = DashboardSection::Home;
constexpr DashboardSection Agents = DashboardSection::Manifests;
constexpr DashboardSection Harness = DashboardSection::Home;
constexpr DashboardSection Runtime = DashboardSection::Home;

enum class DashboardFocus { Dock, Content };

struct DashboardState {
    DashboardSection section = DashboardSection::Home;
    DashboardFocus focus = DashboardFocus::Content;
    int navigationIndex = 0;
    int sessionIndex = 0;
    int manifestIndex = 0;

    // Facets — kind is primary; tag secondary; search tertiary.
    std::string manifestFilter;  // empty = all kinds
    std::string tagFilter;
    std::string searchQuery;
    bool searchMode = false;

    // Last actionable yank (launch cmd) — `y` re-emits to notice.
    std::string yankBuffer;

    std::vector<session::SessionManager::SessionInfo> sessions;
    std::vector<catalog::ManifestEntry> manifests;
    std::vector<catalog::AgentEntry> agents;
    std::string notice;
    std::string manifestDir;

    // Section pill anim
    int navPrevIndex = 0;
    int navAnimDir = 1;
    int64_t navAnimStartMs = 0;
    static constexpr int navAnimDurationMs = 280;
    static constexpr int sectionCount = 4;

    // Manifest card swipe (j/k) — curved dual-card transition
    int cardPrevIndex = -1;
    int cardAnimDir = 1;  // +1 next (j down list), -1 prev
    int64_t cardAnimStartMs = 0;
    static constexpr int cardAnimDurationMs = 360;

    // Kind facet order for 1-9 binding (0/all is unnumbered / `f` or `0`)
    static const std::vector<std::string>& kindFacets() {
        static const std::vector<std::string> k = {
            "", "agent", "tool", "feed", "workflow", "harness", "prompt", "skill", "relic",
        };
        return k;
    }

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

    void beginNavAnim(int fromIdx, int dir) {
        navPrevIndex = fromIdx;
        navAnimDir = dir >= 0 ? 1 : -1;
        navAnimStartMs = nowMs();
    }

    // Always slide-up magnitude (caller may abs).
    int pageSlideRows(int maxRows) const {
        float t = navAnimT();
        if (t >= 1.f || maxRows <= 0) return 0;
        float u = 1.f - t;
        float e = u * u * u;
        return static_cast<int>(e * static_cast<float>(maxRows));
    }

    void syncSection() { section = static_cast<DashboardSection>(navigationIndex); }

    void moveNavigation(int delta) {
        int from = navigationIndex;
        int next = navigationIndex + delta;
        if (next < 0) next = sectionCount - 1;
        if (next >= sectionCount) next = 0;
        if (next == navigationIndex) return;
        beginNavAnim(from, delta >= 0 ? 1 : -1);
        navigationIndex = next;
        syncSection();
    }

    void select(DashboardSection next) {
        int from = navigationIndex;
        int idx = static_cast<int>(next);
        if (idx < 0 || idx >= sectionCount) return;
        if (idx != navigationIndex) beginNavAnim(from, idx > from ? 1 : -1);
        section = next;
        navigationIndex = idx;
        focus = DashboardFocus::Content;
    }

    void toggleFocus() {
        focus = (focus == DashboardFocus::Dock) ? DashboardFocus::Content
                                                : DashboardFocus::Dock;
    }

    void moveSession(int delta) {
        if (sessions.empty()) {
            sessionIndex = 0;
            return;
        }
        sessionIndex =
            std::max(0, std::min(static_cast<int>(sessions.size()) - 1, sessionIndex + delta));
    }

    float cardAnimT() const {
        if (cardAnimStartMs <= 0) return 1.f;
        float t = static_cast<float>(nowMs() - cardAnimStartMs) /
                  static_cast<float>(cardAnimDurationMs);
        if (t < 0.f) return 0.f;
        if (t > 1.f) return 1.f;
        return t;
    }

    bool cardAnimating() const { return cardAnimT() < 1.f && cardPrevIndex >= 0; }

    void beginCardAnim(int fromIdx, int dir) {
        cardPrevIndex = fromIdx;
        cardAnimDir = dir >= 0 ? 1 : -1;
        cardAnimStartMs = nowMs();
    }

    void moveManifest(int delta) {
        if (manifests.empty()) {
            manifestIndex = 0;
            return;
        }
        int from = manifestIndex;
        int next =
            std::max(0, std::min(static_cast<int>(manifests.size()) - 1, manifestIndex + delta));
        if (next != from) beginCardAnim(from, delta >= 0 ? 1 : -1);
        manifestIndex = next;
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
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
        applyManifestFilters(catalog::discoverManifests(manifestDir));
        agents = catalog::discoverAgents(manifestDir);
    }

    void refreshAgents() { refreshManifests(); }

    void refreshAll() {
        refreshSessions();
        refreshManifests();
    }

    void setKindFilter(const std::string& kind) {
        manifestFilter = kind;
        refreshManifests();
    }

    // Cycle kind facet forward.
    void cycleManifestFilter() {
        const auto& facets = kindFacets();
        int idx = 0;
        for (int i = 0; i < static_cast<int>(facets.size()); ++i)
            if (facets[static_cast<size_t>(i)] == manifestFilter) {
                idx = i;
                break;
            }
        setKindFilter(facets[static_cast<size_t>((idx + 1) % facets.size())]);
    }

    // Digit 1..9 → kind facet (1=agent). 0 clears.
    bool applyKindDigit(int digit) {
        if (digit == 0) {
            setKindFilter("");
            return true;
        }
        const auto& facets = kindFacets();
        // facets[0] is "", so digit 1 → facets[1]
        if (digit < 1 || digit >= static_cast<int>(facets.size())) return false;
        setKindFilter(facets[static_cast<size_t>(digit)]);
        return true;
    }

    void cycleTagFilter() {
        auto all = catalog::discoverManifests(manifestDir);
        std::vector<std::string> tags;
        auto add = [&](const std::string& t) {
            if (t.empty()) return;
            if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
        };
        add("");
        for (const auto& m : all) {
            add(m.category);
            for (const auto& t : m.tags) {
                if (t == m.kind || t == "launchable" || t == "prod") continue;
                add(t);
            }
        }
        if (tags.size() > 20) tags.resize(20);
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
            if (m->kind == "agent") {
                for (const auto& a : agents)
                    if (a.manifestPath == m->path) return &a;
            }
        }
        return nullptr;
    }
};

inline const char* dashboardSectionName(DashboardSection section) {
    switch (section) {
        case DashboardSection::Home: return "Home";
        case DashboardSection::Sessions: return "Sessions";
        case DashboardSection::Manifests: return "Manifests";
        case DashboardSection::Help: return "Help";
    }
    return "Home";
}

}  // namespace cortex::mk3::ui::model
