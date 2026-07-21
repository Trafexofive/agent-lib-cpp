#pragma once
// Dashboard hub — frontend treatment.
// Full-bleed elevated content stage + floating rounded pill dock.
// Section cycle: Ctrl-J / Ctrl-K only (inkcell: LF=Ctrl-J, CR=Enter).

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

        // ── Search mode ──────────────────────────────────────────────
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
            return true;
        }

        // ── Ctrl-J / Ctrl-K : section cycle (REQUIRED) ───────────────
        if (event.ctrl() && event.code == KeyCode::Character) {
            if (event.ch == 'j' || event.ch == 'J') {
                dash.moveNavigation(1);
                bumpNotice();
                return true;
            }
            if (event.ch == 'k' || event.ch == 'K') {
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

        // List nav — plain j/k only when NOT ctrl
        if (!event.ctrl() &&
            (event.code == KeyCode::ArrowUp ||
             (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K')))) {
            if (dash.section == model::DashboardSection::Sessions) dash.moveSession(-1);
            else if (dash.section == model::DashboardSection::Manifests) dash.moveManifest(-1);
            return true;
        }
        if (!event.ctrl() &&
            (event.code == KeyCode::ArrowDown ||
             (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J')))) {
            if (dash.section == model::DashboardSection::Sessions) dash.moveSession(1);
            else if (dash.section == model::DashboardSection::Manifests) dash.moveManifest(1);
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
                    if (dash.section == model::DashboardSection::Sessions) deleteSelectedSession();
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
        if (layout::render_min_size_notice(surface, 80, 20)) return;
        surface.clear(theme::base_bg());
        const inkcell::Rect page = layout::page(surface);
        const auto tier = layout::densityOf(page.w);
        const auto& dash = model_->dashboard;

        // ── App bar ──────────────────────────────────────────────────
        drawAppBar(surface, page, tier);

        // ── Stage (elevated rounded panel) ───────────────────────────
        // Leave room: 1 status + 1 gap + 3 pill dock = 5 above bottom
        const int dockReserve = 5;
        int stageTop = page.y + 3;
        int stageH = std::max(6, page.bottom() - dockReserve - stageTop);
        inkcell::Rect stage{page.x, stageTop, page.w, stageH};

        auto stageBg = theme::panel_bg();
        surface.fill(stage, " ", stageBg);
        surface.box(stage, inkcell::BorderStyle::Rounded,
                    stageBg.with_fg(theme::color(inkcell::Color::rgb(48, 48, 48),
                                                 inkcell::Color::rgb(30, 42, 62))));

        // Inner content inset
        inkcell::Rect content{stage.x + 2, stage.y + 1, std::max(1, stage.w - 4),
                              std::max(1, stage.h - 2)};
        drawContent(surface, content);

        // ── Status rail (above pill) ─────────────────────────────────
        int railY = page.bottom() - 4;
        std::string left = dash.searchMode ? ("/" + dash.searchQuery + "█")
                           : dash.notice.empty() ? std::string(model::dashboardSectionName(dash.section))
                                                 : dash.notice;
        components::drawStatusRail(surface, page.x, page.w, railY, left, "ctrl-j/k cycle · j/k list");

        // ── Floating pill dock ───────────────────────────────────────
        static const std::vector<components::PillItem> pills = {
            {"o", "Overview"}, {"s", "Sessions"}, {"a", "Manifests"},
            {"h", "Harness"},  {"r", "Runtime"},  {"?", "Help"},
        };
        components::drawPillDock(surface, page.x, page.w, page.bottom() - 1, pills,
                                 dash.navigationIndex, dash.navPrevIndex, dash.navAnimT());
    }

   private:
    void drawAppBar(inkcell::Surface& surface, inkcell::Rect page, layout::DensityTier tier) const {
        auto bar = theme::panel_2();
        surface.fill({page.x, page.y, page.w, 2}, " ", bar);
        // accent hairline under bar
        surface.hline({page.x, page.y + 2}, page.w, "─",
                      theme::dim().with_fg(theme::color(inkcell::Color::rgb(50, 50, 50),
                                                        inkcell::Color::rgb(28, 40, 58))));

        std::string brand = "CORTEX";
        std::string product = "MK3";
        surface.text({page.x, page.y}, brand, theme::bright().with_bg(bar.bg));
        surface.text({page.x + 7, page.y}, product,
                     theme::cyan().with_bg(bar.bg));
        surface.text({page.x + 11, page.y},
                     "  ·  " + std::string(model::dashboardSectionName(model_->dashboard.section)),
                     theme::text().with_bg(bar.bg));

        std::string right = nonempty(cfg_.agentName, "builtin") + "  ·  " +
                            nonempty(cfg_.provider, "?") + "/" + nonempty(cfg_.model, "?") +
                            "  ·  " + layout::densityName(tier);
        int rw = inkcell::text::display_width(right);
        surface.text({std::max(page.x, page.right() - rw), page.y}, right,
                     theme::dim().with_bg(bar.bg));

        // secondary line: session + registry pulse
        std::string sub = "session " + suffix(model_->activeSessionId) + "   registry " +
                          std::to_string(model_->dashboard.manifests.size()) + "   " +
                          (model_->running ? "● live" : "○ idle");
        surface.text({page.x, page.y + 1}, inkcell::text::truncate(sub, page.w),
                     theme::dim().with_bg(bar.bg));
    }

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
            if (!dash.manifestFilter.empty()) dash.notice += " · " + dash.manifestFilter;
            if (!dash.tagFilter.empty()) dash.notice += " · #" + dash.tagFilter;
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

    void sectionHead(inkcell::Surface& surface, inkcell::Rect frame, const std::string& title,
                     const std::string& subtitle) const {
        surface.text({frame.x, frame.y}, title, theme::bright());
        if (!subtitle.empty())
            surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(subtitle, frame.w),
                         theme::dim());
        // accent underline under title
        int tw = std::min(frame.w, inkcell::text::display_width(title) + 4);
        surface.hline({frame.x, frame.y + 2}, tw, "─", theme::cyan());
    }

    // ── Metric tile ──────────────────────────────────────────────────
    void metricTile(inkcell::Surface& surface, inkcell::Rect r, const std::string& label,
                    const std::string& value, inkcell::Style valueSt) const {
        surface.fill(r, " ", theme::panel_2());
        surface.box(r, inkcell::BorderStyle::Rounded,
                    theme::panel_2().with_fg(theme::color(inkcell::Color::rgb(55, 55, 55),
                                                          inkcell::Color::rgb(32, 44, 64))));
        surface.text({r.x + 2, r.y + 1}, inkcell::text::truncate(label, r.w - 4), theme::dim());
        surface.text({r.x + 2, r.y + 2}, inkcell::text::truncate(value, r.w - 4), valueSt);
    }

    void drawOverview(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionHead(surface, frame, "Overview", "Operator hub · ctrl-j/k cycles the dock");
        int y = frame.y + 4;

        // Metric tiles row
        int tileW = std::max(16, (frame.w - 6) / 4);
        int tileH = 4;
        if (y + tileH < frame.bottom() && frame.w >= 64) {
            metricTile(surface, {frame.x, y, tileW, tileH}, "AGENT",
                       nonempty(cfg_.agentName, "builtin"), theme::bright());
            metricTile(surface, {frame.x + tileW + 2, y, tileW, tileH}, "STATUS",
                       model_->running ? "running" : "ready",
                       model_->running ? theme::green() : theme::text());
            metricTile(surface, {frame.x + 2 * (tileW + 2), y, tileW, tileH}, "REGISTRY",
                       std::to_string(model_->dashboard.manifests.size()), theme::cyan());
            metricTile(surface, {frame.x + 3 * (tileW + 2), y, tileW, tileH}, "SESSIONS",
                       std::to_string(model_->dashboard.sessions.size()), theme::text());
            y += tileH + 2;
        }

        components::fieldLine(surface, frame.x, y++, frame.w, "session",
                              suffix(model_->activeSessionId));
        components::fieldLine(surface, frame.x, y++, frame.w, "manifest",
                              cfg_.manifestPath.empty() ? "builtin" : basename(cfg_.manifestPath));
        components::fieldLine(surface, frame.x, y++, frame.w, "agents",
                              std::to_string(model_->dashboard.agents.size()) + " top-level");
        y += 1;
        surface.text({frame.x, y++}, "DOCK", theme::dim());
        surface.text({frame.x, y++}, "  ctrl-j / ctrl-k     cycle sections (animated pill)",
                     theme::green());
        surface.text({frame.x, y++}, "  a · / · f · t       registry · search · kind · tag",
                     theme::text());
        surface.text({frame.x, y}, "  c                   open chat", theme::text());
    }

    void drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionHead(surface, frame, "Sessions", "enter resume · n new · d delete");
        const auto& dash = model_->dashboard;
        int y = frame.y + 4;
        if (dash.sessions.empty()) {
            surface.text({frame.x, y++}, "No sessions yet.", theme::dim());
            surface.text({frame.x, y}, "n → create and jump to chat.", theme::text());
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

        sectionHead(surface, frame, "Manifests",
                    std::string(layout::densityName(layout::densityOf(frame.w))) + " · " +
                        std::to_string(dash.manifests.size()) + " shown · f kind · t tag · / search");

        int y = frame.y + 4;

        // Kind chips
        std::map<std::string, int> kindCounts;
        for (const auto& m : dash.manifests) kindCounts[m.kind]++;
        static const char* kinds[] = {"agent", "tool", "feed", "workflow", "harness", "prompt", "skill"};
        std::vector<components::Chip> kindChips;
        kindChips.push_back({"", "all", -1, dash.manifestFilter.empty()});
        for (const char* k : kinds)
            kindChips.push_back({k, k, kindCounts.count(k) ? kindCounts[k] : 0,
                                 dash.manifestFilter == k});
        int after = y;
        components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, kindChips, &after);
        y = after;

        // Category chips
        std::map<std::string, int> catCounts;
        for (const auto& m : dash.manifests) catCounts[m.category]++;
        std::vector<components::Chip> catChips;
        for (const auto& kv : catCounts)
            catChips.push_back({kv.first, kv.first, kv.second, dash.tagFilter == kv.first});
        if (!catChips.empty()) {
            components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, catChips, &after);
            y = after + 1;
        } else {
            ++y;
        }

        if (dash.manifests.empty()) {
            bool filtered = !dash.manifestFilter.empty() || !dash.tagFilter.empty() ||
                            !dash.searchQuery.empty();
            surface.text({frame.x, y++},
                         filtered ? "No matches — Esc clears filters." : assets::MANIFESTS_EMPTY,
                         theme::amber());
            if (!filtered) {
                for (const auto& r : catalog::manifestsSearchRoots(dash.manifestDir)) {
                    if (y >= frame.bottom()) break;
                    surface.text({frame.x, y++},
                                 inkcell::text::truncate("  [" + r.second + "] " + r.first, frame.w),
                                 theme::dim());
                }
            }
            return;
        }

        // Grouped rows
        struct Row {
            bool header = false;
            std::string text;
            int idx = -1;
        };
        std::vector<Row> rows;
        std::string last;
        for (int i = 0; i < static_cast<int>(dash.manifests.size()); ++i) {
            const auto& m = dash.manifests[static_cast<size_t>(i)];
            if (m.category != last) {
                last = m.category;
                rows.push_back({true,
                                "▸ " + m.category + "  (" + std::to_string(catCounts[m.category]) + ")",
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

        int listW = L.listW;
        int visible = std::max(1, frame.bottom() - y);
        int start = std::max(0, std::min(selRow - visible / 3,
                                         std::max(0, static_cast<int>(rows.size()) - visible)));

        for (int ri = start; ri < static_cast<int>(rows.size()) && y < frame.bottom(); ++ri) {
            const auto& row = rows[static_cast<size_t>(ri)];
            if (row.header) {
                surface.text({frame.x, y++}, inkcell::text::truncate(row.text, listW), theme::amber());
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
                components::drawTagChips(surface, nameCol + nameBudget + 1, y, L.tagColMax, m.tags, 5);
            }
            surface.text({nameCol, y}, inkcell::text::truncate(name, nameBudget),
                         selected ? theme::bright() : theme::text());
            ++y;
        }

        if (!L.showDetail) return;
        const auto* sel = dash.selectedManifest();
        if (!sel) return;

        inkcell::Rect det{frame.x + L.detailX, frame.y + 4, L.detailW, frame.h - 5};
        surface.fill(det, " ", theme::panel_2());
        surface.box(det, inkcell::BorderStyle::Rounded,
                    theme::panel_2().with_fg(theme::color(inkcell::Color::rgb(55, 55, 55),
                                                          inkcell::Color::rgb(36, 50, 72))));
        int dy = det.y + 1;
        int ix = det.x + 2;
        int iw = det.w - 4;
        surface.text({ix, dy++}, inkcell::text::truncate(sel->name, iw), theme::bright());
        surface.text({ix, dy++},
                     inkcell::text::truncate(std::string(assets::kindLabel(sel->kind)) + " · " +
                                                 sel->category,
                                             iw),
                     theme::cyan());
        if (!sel->summary.empty()) {
            for (const auto& line : chat::wrapWordsLossless(sel->summary, iw)) {
                if (dy >= det.bottom() - 9) break;
                surface.text({ix, dy++}, line, theme::text());
            }
        }
        ++dy;
        components::fieldLine(surface, ix, dy++, iw, "kind", sel->kind);
        components::fieldLine(surface, ix, dy++, iw, "category", sel->category);
        if (!sel->version.empty())
            components::fieldLine(surface, ix, dy++, iw, "version", sel->version);
        if (!sel->provider.empty() || !sel->model.empty())
            components::fieldLine(surface, ix, dy++, iw, "engine",
                                  nonempty(sel->provider, "?") + "/" + nonempty(sel->model, "?"));
        components::fieldLine(surface, ix, dy++, iw, "path",
                              sel->relPath.empty() ? sel->path : sel->relPath);
        ++dy;
        surface.text({ix, dy++}, "TAGS", theme::dim());
        std::string all;
        for (const auto& t : sel->tags) {
            if (!all.empty()) all += "  ";
            all += "#" + t;
        }
        for (const auto& line : chat::wrapWordsLossless(all.empty() ? "—" : all, iw)) {
            if (dy >= det.bottom() - 3) break;
            surface.text({ix, dy++}, line, theme::text());
        }
        ++dy;
        if (sel->launchable && sel->kind == "agent") {
            surface.text({ix, dy++}, "LAUNCH", theme::green());
            std::string cmd = sel->nested ? ("cortex-mk3 -m " + sel->path)
                                          : ("cortex-mk3 -m " + sel->name);
            cmd += " --tui experimental";
            surface.text({ix, dy}, inkcell::text::truncate(cmd, iw), theme::green());
        }
    }

    void drawHarness(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionHead(surface, frame, "Harness", "prompt stack + live surface");
        int y = frame.y + 4;
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
        auto block = [&](const char* label, const std::vector<std::string>& names) {
            if (y >= frame.bottom()) return;
            surface.text({frame.x, y++},
                         std::string(label) + "  (" + std::to_string(names.size()) + ")",
                         theme::dim());
            std::string joined;
            for (const auto& n : names) {
                if (!joined.empty()) joined += " · ";
                joined += n;
            }
            for (const auto& line :
                 chat::wrapWordsLossless(joined.empty() ? "none" : joined, frame.w - 2)) {
                if (y >= frame.bottom()) break;
                surface.text({frame.x + 2, y++}, line,
                             names.empty() ? theme::dim() : theme::text());
            }
            ++y;
        };
        block("TOOLS", model_->rootAgent->toolNames());
        block("SUB-AGENTS", model_->rootAgent->subAgentNames());
        block("FEEDS", model_->rootAgent->feedNames());
        block("RELICS", model_->rootAgent->relicNames());
    }

    void drawRuntime(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionHead(surface, frame, "Runtime", "process + provider");
        int y = frame.y + 4;
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
        sectionHead(surface, frame, "Help", "dock · registry · global");
        int y = frame.y + 4;
        const char* lines[] = {
            "DOCK",
            "  ctrl-j / ctrl-k    cycle sections · sliding thumb + glow",
            "  o s a h r ?        jump",
            "",
            "REGISTRY",
            "  j/k                move",
            "  f / t              kind chips / tag chips",
            "  /                  search  ·  Esc clears layers",
            "  enter              launch hint (agents)",
            "",
            "GLOBAL",
            "  c chat · n/d session · R refresh · T theme · q quit",
            "",
            "Density  narrow<100  standard  wide≥160 (list|detail)",
            "Note: Enter is CR; Ctrl-J is LF — decoded separately in inkcell.",
        };
        for (const char* line : lines) {
            if (y >= frame.bottom()) break;
            bool head = line[0] && line[0] != ' ' &&
                        std::isupper(static_cast<unsigned char>(line[0]));
            surface.text({frame.x, y++}, line,
                         !line[0] ? theme::dim() : head ? theme::cyan() : theme::text());
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
                    dash.notice = m->nested ? ("launch: cortex-mk3 -m " + m->path)
                                            : ("launch: cortex-mk3 -m " + m->name);
                    dash.notice += " --tui experimental";
                } else {
                    dash.notice = m->kind + " · #" + (m->tags.empty() ? m->category : m->tags.front());
                }
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
