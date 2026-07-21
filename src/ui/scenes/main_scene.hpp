#pragma once
// Dashboard hub — full-bleed content + floating bottom pill dock.
// Frontend-grade density (narrow/standard/wide), category chips, tags, /.
// Cortex-local chrome only.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "base_scene.hpp"
#include "src/session/manager.hpp"
#include "src/ui/assets/glyphs.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/components/chips.hpp"
#include "src/ui/components/chrome.hpp"
#include "src/ui/components/pill_nav.hpp"
#include "src/ui/layout/density.hpp"
#include "src/ui/model/dashboard_controller.hpp"

namespace cortex::mk3::ui::scenes {

class MainScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Dashboard"; }

    void on_enter() override {
        BaseScene::on_enter();
        if (!cfg_.manifestDir.empty())
            model_->dashboard.manifestDir = cfg_.manifestDir;
        else if (!cfg_.manifestPath.empty())
            model_->dashboard.manifestDir = cfg_.manifestPath;
        else
            model_->dashboard.manifestDir.clear();
        model_->dashboard.refreshAll();
        model_->dashboard.focus = model::DashboardFocus::Content;
        model_->dashboard.searchMode = false;
        bumpNotice();
        highlightActiveAgent();
    }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;
        auto& dash = model_->dashboard;

        // ── Search mode (/) ──────────────────────────────────────────
        if (dash.searchMode) {
            if (event.code == KeyCode::Escape) {
                dash.searchMode = false;
                dash.searchQuery.clear();
                dash.refreshManifests();
                bumpNotice();
                return true;
            }
            if (event.code == KeyCode::Enter) {
                dash.searchMode = false;
                bumpNotice();
                return true;
            }
            if (event.code == KeyCode::Backspace) {
                if (!dash.searchQuery.empty()) dash.searchQuery.pop_back();
                dash.refreshManifests();
                bumpNotice();
                return true;
            }
            if (event.code == KeyCode::Character && !event.ctrl() && event.ch >= 32) {
                dash.searchQuery.push_back(static_cast<char>(event.ch));
                dash.refreshManifests();
                bumpNotice();
                return true;
            }
            return true;  // trap while searching
        }

        // ── Section cycle ────────────────────────────────────────────
        // Classic TTY: Ctrl-J == LF == Enter — cannot bind Ctrl-J reliably.
        // Primary: Ctrl-N / Ctrl-P  (and Ctrl-K = prev). Also [ ] .
        if (event.ctrl() && event.code == KeyCode::Character) {
            if (event.ch == 'n' || event.ch == 'N' || event.ch == 'j' || event.ch == 'J') {
                dash.moveNavigation(1);
                bumpNotice();
                return true;
            }
            if (event.ch == 'p' || event.ch == 'P' || event.ch == 'k' || event.ch == 'K') {
                dash.moveNavigation(-1);
                bumpNotice();
                return true;
            }
        }
        if (event.ctrl() && event.code == KeyCode::ArrowDown) {
            dash.moveNavigation(1);
            bumpNotice();
            return true;
        }
        if (event.ctrl() && event.code == KeyCode::ArrowUp) {
            dash.moveNavigation(-1);
            bumpNotice();
            return true;
        }
        if (!event.ctrl() && event.code == KeyCode::Character) {
            if (event.ch == ']') {
                dash.moveNavigation(1);
                bumpNotice();
                return true;
            }
            if (event.ch == '[') {
                dash.moveNavigation(-1);
                bumpNotice();
                return true;
            }
        }

        if (event.code == KeyCode::Escape) {
            if (!dash.searchQuery.empty()) {
                dash.searchQuery.clear();
                dash.refreshManifests();
                dash.notice = "search cleared";
                return true;
            }
            if (!dash.tagFilter.empty()) {
                dash.tagFilter.clear();
                dash.refreshManifests();
                dash.notice = "tag cleared";
                return true;
            }
            if (!dash.manifestFilter.empty()) {
                dash.manifestFilter.clear();
                dash.refreshManifests();
                dash.notice = "kind cleared";
                return true;
            }
            return true;
        }

        // Content list nav
        if (!event.ctrl() &&
            (event.code == KeyCode::ArrowUp ||
             (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K')))) {
            if (dash.section == model::DashboardSection::Sessions)
                dash.moveSession(-1);
            else if (dash.section == model::DashboardSection::Manifests)
                dash.moveManifest(-1);
            return true;
        }
        if (!event.ctrl() &&
            (event.code == KeyCode::ArrowDown ||
             (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J')))) {
            if (dash.section == model::DashboardSection::Sessions)
                dash.moveSession(1);
            else if (dash.section == model::DashboardSection::Manifests)
                dash.moveManifest(1);
            return true;
        }

        if (event.code == KeyCode::Enter) {
            activate();
            return true;
        }

        if (event.code == KeyCode::Character && !event.ctrl()) {
            switch (event.ch) {
                case '/':
                    if (dash.section == model::DashboardSection::Manifests) {
                        dash.searchMode = true;
                        dash.notice = "search: ";
                        return true;
                    }
                    break;
                case 'c':
                case 'C':
                    model_->pendingRoute = "agent";
                    return true;
                case 'o':
                case 'O':
                    dash.select(model::DashboardSection::Overview);
                    bumpNotice();
                    return true;
                case 's':
                case 'S':
                    dash.select(model::DashboardSection::Sessions);
                    bumpNotice();
                    return true;
                case 'a':
                case 'A':
                case 'm':
                case 'M':
                    dash.select(model::DashboardSection::Manifests);
                    dash.refreshManifests();
                    bumpNotice();
                    return true;
                case 'f':
                case 'F':
                    if (dash.section == model::DashboardSection::Manifests) {
                        dash.cycleManifestFilter();
                        bumpNotice();
                    }
                    return true;
                case 't':
                    if (dash.section == model::DashboardSection::Manifests) {
                        dash.cycleTagFilter();
                        bumpNotice();
                    }
                    return true;
                case 'h':
                case 'H':
                    dash.select(model::DashboardSection::Harness);
                    bumpNotice();
                    return true;
                case 'r':
                    dash.select(model::DashboardSection::Runtime);
                    bumpNotice();
                    return true;
                case '?':
                    dash.select(model::DashboardSection::Help);
                    bumpNotice();
                    return true;
                case 'n':
                case 'N':
                    if (dash.section == model::DashboardSection::Sessions ||
                        dash.section == model::DashboardSection::Overview)
                        createSession();
                    return true;
                case 'd':
                case 'D':
                    if (dash.section == model::DashboardSection::Sessions)
                        deleteSelectedSession();
                    return true;
                case 'R':
                    dash.refreshAll();
                    dash.notice = "refreshed";
                    return true;
                case 'T':
                    theme::toggle();
                    return true;
                case 'q':
                case 'Q':
                    model_->pendingRoute = "quit";
                    return true;
            }
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface, 80, 18)) return;
        surface.clear(theme::base_bg());
        inkcell::Rect page = layout::page(surface);
        auto tier = layout::densityOf(page.w);

        // Top app bar
        std::string left = std::string(assets::DASH_TITLE) + "  ·  " +
                           model::dashboardSectionName(model_->dashboard.section);
        std::string right = nonempty(cfg_.agentName, "builtin") + "  ·  " +
                            nonempty(cfg_.provider, "?") + "/" + nonempty(cfg_.model, "?") +
                            "  ·  " + layout::densityName(tier);
        components::headerStrip(surface, {page.x, page.y, page.w, 2}, left, right);

        // Dock stack: status rail + glow + pill = 3 rows
        const int dockH = 3;
        int top = page.y + 3;
        int contentH = std::max(1, page.bottom() - dockH - top);
        inkcell::Rect content{page.x, top, page.w, contentH};
        drawContent(surface, content);

        const auto& dash = model_->dashboard;
        std::string mid = dash.searchMode ? ("/" + dash.searchQuery + "▌")
                          : dash.notice.empty() ? std::string("hub")
                                                : dash.notice;
        components::drawStatusRail(surface, page.x, page.w, page.bottom() - 3, mid,
                                   "^N/^P cycle  [ ] also", "j/k list · / search · q");

        static const std::vector<components::PillItem> pills = {
            {"o", "Overview"}, {"s", "Sessions"}, {"a", "Manifests"},
            {"h", "Harness"},  {"r", "Runtime"},  {"?", "Help"},
        };
        components::drawPillDock(surface, page.x, page.w, page.bottom() - 1, pills,
                                 dash.navigationIndex, dash.navPrevIndex, dash.navAnimT());
    }

   private:
    void highlightActiveAgent() {
        if (cfg_.agentName.empty() && cfg_.manifestPath.empty()) return;
        for (int i = 0; i < static_cast<int>(model_->dashboard.manifests.size()); ++i) {
            const auto& m = model_->dashboard.manifests[static_cast<size_t>(i)];
            if (m.kind != "agent") continue;
            if ((!cfg_.agentName.empty() && m.name == cfg_.agentName) ||
                (!cfg_.manifestPath.empty() && m.path == cfg_.manifestPath)) {
                model_->dashboard.manifestIndex = i;
                break;
            }
        }
    }

    void bumpNotice() {
        auto& dash = model_->dashboard;
        if (dash.searchMode) {
            dash.notice = "search: " + dash.searchQuery;
            return;
        }
        if (dash.section == model::DashboardSection::Manifests) {
            dash.notice = std::to_string(dash.manifests.size()) + " shown";
            if (!dash.manifestFilter.empty()) dash.notice += " · kind=" + dash.manifestFilter;
            if (!dash.tagFilter.empty()) dash.notice += " · tag=" + dash.tagFilter;
            if (!dash.searchQuery.empty()) dash.notice += " · /" + dash.searchQuery;
        } else if (dash.section == model::DashboardSection::Sessions) {
            dash.notice = std::to_string(dash.sessions.size()) + " sessions";
        } else {
            dash.notice = model::dashboardSectionName(dash.section);
        }
    }

    static std::string suffix(const std::string& value) {
        if (value.empty()) return "none";
        return value.size() > 14 ? value.substr(value.size() - 14) : value;
    }
    static std::string basename(const std::string& path) {
        if (path.empty()) return "none";
        size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    void drawContent(inkcell::Surface& surface, inkcell::Rect frame) const {
        switch (model_->dashboard.section) {
            case model::DashboardSection::Overview: drawOverview(surface, frame); break;
            case model::DashboardSection::Sessions: drawSessions(surface, frame); break;
            case model::DashboardSection::Manifests: drawManifests(surface, frame); break;
            case model::DashboardSection::Harness: drawHarness(surface, frame); break;
            case model::DashboardSection::Runtime: drawRuntime(surface, frame); break;
            case model::DashboardSection::Help: drawHelp(surface, frame); break;
        }
    }

    void sectionTitle(inkcell::Surface& surface, inkcell::Rect frame, const std::string& title,
                      const std::string& subtitle) const {
        components::sectionHead(surface, {frame.x, frame.y, frame.w, 2}, title, subtitle);
        components::hairline(surface, frame.x, frame.y + 3, frame.w, theme::dim());
    }

    void drawOverview(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Overview",
                     "Control hub · floating pill dock · PROD manifests registry");
        int y = frame.y + 5;
        // Metric cards row
        auto card = [&](int x, const char* label, const std::string& value, inkcell::Style st) {
            surface.text({x, y}, label, theme::dim());
            surface.text({x, y + 1}, inkcell::text::truncate(value, 18), st);
        };
        card(frame.x, "AGENT", nonempty(cfg_.agentName, "builtin"), theme::bright());
        card(frame.x + 22, "STATUS", model_->running ? "running" : "ready",
             model_->running ? theme::green() : theme::text());
        card(frame.x + 44, "REGISTRY",
             std::to_string(model_->dashboard.manifests.size()) + " entries", theme::cyan());
        if (frame.w >= 90)
            card(frame.x + 66, "SESSIONS", std::to_string(model_->dashboard.sessions.size()),
                 theme::text());
        y += 3;
        components::hairline(surface, frame.x, y++, frame.w, theme::dim());
        components::fieldLine(surface, frame.x, y++, frame.w, "session",
                              suffix(model_->activeSessionId));
        components::fieldLine(surface, frame.x, y++, frame.w, "manifest",
                              cfg_.manifestPath.empty() ? "builtin" : basename(cfg_.manifestPath));
        components::fieldLine(surface, frame.x, y++, frame.w, "top-level agents",
                              std::to_string(model_->dashboard.agents.size()));
        y += 1;
        surface.text({frame.x, y++}, "DOCK", theme::dim());
        surface.text({frame.x, y++}, "  Ctrl-N / Ctrl-P   cycle pill (Ctrl-J = Enter on TTY)",
                     theme::green());
        surface.text({frame.x, y++}, "  [  ]              cycle pill too", theme::text());
        surface.text({frame.x, y++}, "  a  /  f  t        manifests · kind · tag", theme::text());
        surface.text({frame.x, y++}, "  /                search registry", theme::text());
        surface.text({frame.x, y}, "  c                open chat", theme::text());
    }

    void drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Sessions", "Enter resume · n new · d delete · R refresh");
        const auto& dash = model_->dashboard;
        int y = frame.y + 5;
        if (dash.sessions.empty()) {
            surface.text({frame.x, y++}, "No saved sessions.", theme::dim());
            surface.text({frame.x, y}, "Press n → clean session → chat.", theme::text());
            return;
        }
        int visible = std::max(1, frame.bottom() - y);
        int start = std::max(0, std::min(dash.sessionIndex - visible / 2,
                                         static_cast<int>(dash.sessions.size()) - visible));
        for (int i = start; i < static_cast<int>(dash.sessions.size()) && y < frame.bottom(); ++i) {
            const auto& s = dash.sessions[static_cast<size_t>(i)];
            bool selected = i == dash.sessionIndex;
            components::drawCardRow(surface, {frame.x, y, frame.w, 1}, selected, false);
            std::string line = "  " + suffix(s.id) + "  " + nonempty(s.agentName, "?") + "  " +
                               std::to_string(s.turnCount) + "r  " + s.updated;
            surface.text({frame.x + 1, y}, inkcell::text::truncate(line, frame.w - 2),
                         selected ? theme::bright() : theme::text());
            ++y;
        }
    }

    void drawManifests(inkcell::Surface& surface, inkcell::Rect frame) const {
        const auto& dash = model_->dashboard;
        auto L = layout::manifestLayoutFor(frame.w);

        std::string kind = dash.manifestFilter.empty() ? "all" : dash.manifestFilter;
        std::string tag = dash.tagFilter.empty() ? "all" : dash.tagFilter;
        sectionTitle(surface, frame, "Manifests",
                     std::string("PROD registry · ") + layout::densityName(layout::densityOf(frame.w)) +
                         " · kind=" + kind + " · tag=" + tag +
                         (dash.searchQuery.empty() ? "" : " · /" + dash.searchQuery));

        int y = frame.y + 5;

        // ── Kind chip strip ──
        std::map<std::string, int> kindCounts;
        // counts from unfiltered would be nicer; approximate from current+note
        for (const auto& m : dash.manifests) kindCounts[m.kind]++;
        // Always show full kind palette
        static const char* kinds[] = {"agent", "tool", "feed", "workflow", "harness", "prompt", "skill"};
        std::vector<components::Chip> kindChips;
        kindChips.push_back({"", "all", -1, dash.manifestFilter.empty()});
        for (const char* k : kinds) {
            int c = kindCounts.count(k) ? kindCounts[k] : 0;
            // show even zero when filter active elsewhere
            kindChips.push_back({k, k, c, dash.manifestFilter == k});
        }
        int afterChips = y;
        components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, kindChips, &afterChips);
        y = afterChips;

        // ── Category chip strip ──
        std::map<std::string, int> catCounts;
        for (const auto& m : dash.manifests) catCounts[m.category]++;
        std::vector<components::Chip> catChips;
        catChips.push_back({"", "· categories", -1, dash.tagFilter.empty()});
        for (const auto& kv : catCounts) {
            catChips.push_back({kv.first, kv.first, kv.second, dash.tagFilter == kv.first});
        }
        components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, catChips, &afterChips);
        y = afterChips + 1;

        if (dash.manifests.empty()) {
            bool filtered = !dash.manifestFilter.empty() || !dash.tagFilter.empty() ||
                            !dash.searchQuery.empty();
            surface.text({frame.x, y++},
                         filtered ? "Empty filter — Esc clears search/tag/kind."
                                  : assets::MANIFESTS_EMPTY,
                         theme::amber());
            if (!filtered) {
                auto roots = catalog::manifestsSearchRoots(dash.manifestDir);
                surface.text({frame.x, y++}, "Roots:", theme::dim());
                for (const auto& r : roots) {
                    if (y >= frame.bottom()) break;
                    surface.text({frame.x, y++},
                                 inkcell::text::truncate("  [" + r.second + "] " + r.first, frame.w),
                                 theme::dim());
                }
            }
            return;
        }

        // ── Grouped list (+ optional detail) ──
        struct Row {
            bool header = false;
            std::string headerText;
            int idx = -1;
        };
        std::vector<Row> rows;
        std::string lastCat;
        for (int i = 0; i < static_cast<int>(dash.manifests.size()); ++i) {
            const auto& m = dash.manifests[static_cast<size_t>(i)];
            if (m.category != lastCat) {
                lastCat = m.category;
                rows.push_back(
                    {true, "▸ " + m.category + "  (" + std::to_string(catCounts[m.category]) + ")",
                     -1});
            }
            rows.push_back({false, "", i});
        }

        int selRow = 0;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (!rows[static_cast<size_t>(i)].header &&
                rows[static_cast<size_t>(i)].idx == dash.manifestIndex) {
                selRow = i;
                break;
            }

        int listBottom = frame.bottom();
        int visible = std::max(1, listBottom - y);
        // 2-line cards on wide when detail shown eats height — keep 1-line for density
        int start = std::max(0, std::min(selRow - visible / 3,
                                         std::max(0, static_cast<int>(rows.size()) - visible)));

        int listW = L.listW;
        for (int ri = start; ri < static_cast<int>(rows.size()) && y < listBottom; ++ri) {
            const auto& row = rows[static_cast<size_t>(ri)];
            if (row.header) {
                surface.text({frame.x, y++}, inkcell::text::truncate(row.headerText, listW),
                             theme::amber());
                continue;
            }
            const auto& m = dash.manifests[static_cast<size_t>(row.idx)];
            bool selected = row.idx == dash.manifestIndex;
            bool active = m.kind == "agent" &&
                          ((!cfg_.agentName.empty() && m.name == cfg_.agentName) ||
                           (!cfg_.manifestPath.empty() && m.path == cfg_.manifestPath));

            components::drawCardRow(surface, {frame.x, y, listW, 1}, selected, active);
            components::kindChip(surface, frame.x + 2, y, m.kind, selected);

            std::string name = std::string(active ? "● " : "  ") + m.name;
            if (!m.version.empty()) name += "  v" + m.version;

            int nameCol = frame.x + 8;
            int nameBudget = listW - 10;
            if (L.showTagColumn && L.tagColMax > 0) {
                nameBudget = std::max(10, listW - 10 - L.tagColMax);
                components::drawTagChips(surface, nameCol + nameBudget + 1, y, L.tagColMax, m.tags,
                                         5);
            }
            surface.text({nameCol, y}, inkcell::text::truncate(name, nameBudget),
                         selected ? theme::bright() : theme::text());
            ++y;
        }

        // Detail pane
        if (!L.showDetail) return;
        const auto* sel = dash.selectedManifest();
        if (!sel) return;

        inkcell::Rect det{frame.x + L.detailX, frame.y + 5, L.detailW, frame.h - 6};
        components::fillRect(surface, det, theme::panel_2());
        components::accentBar(surface, det.x, det.y, det.h, theme::footer_accent_idle());

        int dy = det.y + 1;
        surface.text({det.x + 2, dy++}, inkcell::text::truncate(sel->name, det.w - 3),
                     theme::bright());
        surface.text({det.x + 2, dy++},
                     inkcell::text::truncate(std::string(assets::kindLabel(sel->kind)) + "  ·  " +
                                                 sel->category +
                                                 (sel->nested ? "  ·  nested" : "") +
                                                 (sel->builtin ? "  ·  builtin" : ""),
                                             det.w - 3),
                     theme::cyan());
        if (!sel->summary.empty()) {
            for (const auto& line : chat::wrapWordsLossless(sel->summary, det.w - 4)) {
                if (dy >= det.bottom() - 10) break;
                surface.text({det.x + 2, dy++}, line, theme::text());
            }
        }
        dy += 1;
        components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "kind", sel->kind);
        components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "category", sel->category);
        if (!sel->version.empty())
            components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "version", sel->version);
        if (!sel->provider.empty() || !sel->model.empty())
            components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "engine",
                                  nonempty(sel->provider, "?") + "/" + nonempty(sel->model, "?"));
        components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "source", sel->source);
        components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "path",
                              sel->relPath.empty() ? sel->path : sel->relPath);
        dy += 1;
        surface.text({det.x + 2, dy++}, "TAGS", theme::dim());
        {
            std::string all;
            for (const auto& t : sel->tags) {
                if (!all.empty()) all += "  ";
                all += "#" + t;
            }
            for (const auto& line : chat::wrapWordsLossless(all.empty() ? "—" : all, det.w - 4)) {
                if (dy >= det.bottom() - 4) break;
                surface.text({det.x + 2, dy++}, line, theme::text());
            }
        }
        dy += 1;
        if (sel->launchable && sel->kind == "agent") {
            surface.text({det.x + 2, dy++}, "LAUNCH", theme::green());
            std::string cmd =
                sel->nested ? ("cortex-mk3 -m " + sel->path + " --tui experimental")
                            : ("cortex-mk3 -m " + sel->name + " --tui experimental");
            surface.text({det.x + 2, dy++}, inkcell::text::truncate(cmd, det.w - 3), theme::green());
            surface.text({det.x + 2, dy}, "Enter → copy hint to status rail", theme::dim());
        } else {
            surface.text({det.x + 2, dy++}, "INSPECT", theme::amber());
            surface.text({det.x + 2, dy}, "per-kind runtime renderer next (workflows)",
                         theme::dim());
        }
    }

    void drawHarness(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Harness", "Prompt stack + live capability surface");
        int y = frame.y + 5;
        components::fieldLine(surface, frame.x, y++, frame.w, "manifest",
                              cfg_.manifestPath.empty() ? "none" : cfg_.manifestPath);
        components::fieldLine(surface, frame.x, y++, frame.w, "harness", basename(cfg_.harnessPath));
        components::fieldLine(surface, frame.x, y++, frame.w, "system",
                              basename(cfg_.systemPromptPath));
        components::fieldLine(surface, frame.x, y++, frame.w, "persona",
                              basename(cfg_.personaPath));
        y += 1;
        if (!model_->rootAgent) {
            surface.text({frame.x, y}, "No agent runtime bound.", theme::dim());
            return;
        }
        auto renderNames = [&](const std::string& label, const std::vector<std::string>& names) {
            if (y >= frame.bottom()) return;
            surface.text({frame.x, y++}, label + "  (" + std::to_string(names.size()) + ")",
                         theme::dim());
            std::string joined;
            for (const auto& name : names) {
                if (!joined.empty()) joined += " · ";
                joined += name;
            }
            for (const auto& line :
                 chat::wrapWordsLossless(joined.empty() ? "none" : joined, frame.w - 2)) {
                if (y >= frame.bottom()) break;
                surface.text({frame.x + 2, y++}, line,
                             names.empty() ? theme::dim() : theme::text());
            }
            ++y;
        };
        renderNames("TOOLS", model_->rootAgent->toolNames());
        renderNames("SUB-AGENTS", model_->rootAgent->subAgentNames());
        renderNames("FEEDS", model_->rootAgent->feedNames());
        renderNames("RELICS", model_->rootAgent->relicNames());
    }

    void drawRuntime(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Runtime", "Process + provider");
        int y = frame.y + 5;
        components::fieldLine(surface, frame.x, y++, frame.w, "provider",
                              nonempty(cfg_.provider, "unset"));
        components::fieldLine(surface, frame.x, y++, frame.w, "model",
                              nonempty(cfg_.model, "default"));
        components::fieldLine(surface, frame.x, y++, frame.w, "theme", theme::name());
        components::fieldLine(surface, frame.x, y++, frame.w, "lifecycle",
                              cfg_.noSession ? "no-session"
                              : cfg_.ephemeral ? "ephemeral"
                                               : "persistent");
        components::fieldLine(surface, frame.x, y++, frame.w, "turn",
                              model_->running ? "running" : "idle");
        components::fieldLine(surface, frame.x, y++, frame.w, "actions",
                              std::to_string(model_->actionCount));
        components::fieldLine(surface, frame.x, y++, frame.w, "results",
                              std::to_string(model_->resultCount));
    }

    void drawHelp(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Help", "Hub · dock · registry");
        int y = frame.y + 5;
        const char* lines[] = {
            "SECTION DOCK (bottom center floating pill)",
            "  Ctrl-N / Ctrl-P     next / prev section + highlight slide",
            "  Ctrl-K              prev (Ctrl-J is Enter on classic TTY)",
            "  [  ]                next / prev without modifiers",
            "  o s a h r ?         jump",
            "",
            "MANIFESTS REGISTRY",
            "  j/k                 move selection",
            "  f                   cycle kind filter (chip strip)",
            "  t                   cycle tag/category filter",
            "  /                   search name·summary·tags·path",
            "  Esc                 clear search → tag → kind",
            "  Enter               launch hint (agents) / inspect note",
            "",
            "GLOBAL",
            "  c                   chat   ·  R refresh  ·  T theme  ·  q quit",
            "",
            "Density: narrow <100 · standard 100–159 · wide ≥160 (list|detail)",
            "UI chrome lives in src/ui/{assets,components,layout} — not inkcell.",
        };
        for (const char* line : lines) {
            if (y >= frame.bottom()) break;
            bool head = line[0] && line[0] != ' ' && std::isupper(static_cast<unsigned char>(line[0]));
            surface.text({frame.x, y++}, line,
                         line[0] == '\0' ? theme::dim() : head ? theme::cyan() : theme::text());
        }
    }

    void activate() {
        auto& dash = model_->dashboard;
        if (dash.section == model::DashboardSection::Sessions) {
            resumeSelectedSession();
            return;
        }
        if (dash.section == model::DashboardSection::Manifests) {
            if (const auto* m = dash.selectedManifest()) {
                if (m->launchable && m->kind == "agent") {
                    dash.notice =
                        m->nested
                            ? ("launch: cortex-mk3 -m " + m->path + " --tui experimental")
                            : ("launch: cortex-mk3 -m " + m->name + " --tui experimental");
                } else {
                    dash.notice = m->kind + " · " + m->category + " · " +
                                  (m->tags.empty() ? m->name : ("#" + m->tags.front()));
                }
            } else {
                dash.notice = "no selection";
            }
            return;
        }
        if (dash.section == model::DashboardSection::Overview) model_->pendingRoute = "agent";
    }

    void resumeSelectedSession() {
        if (!model_->rootAgent) {
            model_->dashboard.notice = "agent runtime unavailable";
            return;
        }
        session::SessionManager sessions;
        auto result = model::resumeDashboardSession(
            model_->dashboard, sessions,
            [&](const std::string& id) { model_->rootAgent->loadSession(id); });
        if (!result.ok) return;
        model_->loadSessionRecords(result.records);
        model_->activeSessionId = result.sessionId;
        model_->pendingRoute = "agent";
    }

    void createSession() {
        if (!model_->rootAgent) {
            model_->dashboard.notice = "agent runtime unavailable";
            return;
        }
        session::SessionManager sessions;
        auto result = model::createDashboardSession(
            model_->dashboard, sessions, model_->rootAgent->name(), cfg_.model, cfg_.provider,
            [&] { model_->rootAgent->clearHistory(); });
        if (!result.ok) return;
        model_->clearTranscript();
        model_->activeSessionId = result.sessionId;
        model_->pendingRoute = "agent";
    }

    void deleteSelectedSession() {
        const auto* sel = model_->dashboard.selectedSession();
        if (!sel) {
            model_->dashboard.notice = "no session selected";
            return;
        }
        std::string id = sel->id;
        try {
            session::SessionManager sessions;
            sessions.remove(id);
            if (model_->activeSessionId == id) {
                model_->activeSessionId.clear();
                model_->clearTranscript();
                if (model_->rootAgent) model_->rootAgent->clearHistory();
            }
            model_->dashboard.refreshSessions(sessions);
            model_->dashboard.notice = "deleted " + id;
        } catch (const std::exception& e) {
            model_->dashboard.notice = std::string("delete failed: ") + e.what();
        }
    }

    void handle(const inkcell::Action& action) override {
        if (action.is("scroll.up")) model_->dashboard.moveNavigation(-1);
        else if (action.is("scroll.down")) model_->dashboard.moveNavigation(1);
    }
};

}  // namespace cortex::mk3::ui::scenes
