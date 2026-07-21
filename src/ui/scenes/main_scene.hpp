#pragma once
// Dashboard hub — tight IA, operable facets, real agent launch.
// Pill: Home · Sessions · Manifests · Settings
// ctrl-j = prev · ctrl-k = next (inverted). Enter on agent = hot-swap launch.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "base_scene.hpp"
#include "src/session/manager.hpp"
#include "src/ui/assets/glyphs.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/components/card_swipe.hpp"
#include "src/ui/components/chips.hpp"
#include "src/ui/components/chrome.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/components/pill_nav.hpp"
#include "src/ui/gfx/blit.hpp"
#include "src/ui/gfx/field_raster.hpp"
#include "src/ui/gfx/shaders_dedsec.hpp"
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
        else if (!activeManifest().empty())
            model_->dashboard.manifestDir = activeManifest();
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

        // Palette has priority when open / closing input sink
        if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
            std::string action;
            if (components::handleCmdPaletteKey(model_->cmdPalette, event, &action)) {
                if (!action.empty()) runPaletteAction(action);
                return true;
            }
        }

        // Ctrl-P opens palette
        if (event.code == KeyCode::Character && event.ctrl() &&
            (event.ch == 'p' || event.ch == 'P')) {
            model_->cmdPalette.toggle(components::hubCommands());
            return true;
        }

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

        // Tab toggles dock vs content focus
        if (event.code == KeyCode::Tab) {
            dash.toggleFocus();
            return true;
        }

        // ── ctrl-j = prev · ctrl-k = next ────────────────────────────
        if (event.ctrl() && event.code == KeyCode::Character) {
            if (event.ch == 'j' || event.ch == 'J') {
                dash.moveNavigation(-1);
                bumpNotice();
                return true;
            }
            if (event.ch == 'k' || event.ch == 'K') {
                dash.moveNavigation(1);
                bumpNotice();
                return true;
            }
        }

        // When dock focused, plain j/k also cycle (no chord needed)
        if (dash.focus == model::DashboardFocus::Dock && !event.ctrl() &&
            event.code == KeyCode::Character) {
            if (event.ch == 'j' || event.ch == 'J') {
                dash.moveNavigation(-1);
                bumpNotice();
                return true;
            }
            if (event.ch == 'k' || event.ch == 'K') {
                dash.moveNavigation(1);
                bumpNotice();
                return true;
            }
        }

        if (event.code == KeyCode::Escape) {
            if (dash.focus == model::DashboardFocus::Dock) {
                dash.focus = model::DashboardFocus::Content;
                return true;
            }
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
        if (dash.focus == model::DashboardFocus::Content && !event.ctrl() &&
            (event.code == KeyCode::ArrowUp ||
             (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K')))) {
            if (dash.section == model::DashboardSection::Sessions) dash.moveSession(-1);
            else if (dash.section == model::DashboardSection::Manifests) dash.moveManifest(-1);
            return true;
        }
        if (dash.focus == model::DashboardFocus::Content && !event.ctrl() &&
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
            // Leader-leader: space space → palette (not in search)
            if (event.ch == ' ') {
                if (model_->cmdPalette.noteSpace()) {
                    model_->cmdPalette.show(components::hubCommands());
                    return true;
                }
                // single space swallowed on hub (no type-ahead surface)
                return true;
            }
            model_->cmdPalette.clearLeader();

            // Settings: 0 = shader off, 1-7 = pick field shader
            if (dash.section == model::DashboardSection::Settings && event.ch >= '0' &&
                event.ch <= '9') {
                int d = event.ch - '0';
                if (d == 0) {
                    gfx::setFieldEnabled(false);
                    dash.notice = "shader · off";
                } else if (d >= 1 && d <= gfx::fieldCount()) {
                    gfx::setFieldEnabled(true);
                    gfx::setFieldIndex(d - 1);
                    dash.notice = std::string("shader · ") + gfx::activeFieldName();
                }
                return true;
            }
            // Kind digits on Manifests — operable facets
            if (dash.section == model::DashboardSection::Manifests && event.ch >= '0' &&
                event.ch <= '9') {
                if (dash.applyKindDigit(event.ch - '0')) {
                    bumpNotice();
                    return true;
                }
            }
            switch (event.ch) {
                case '/':
                    if (dash.section == model::DashboardSection::Manifests) {
                        dash.searchMode = true;
                        dash.focus = model::DashboardFocus::Content;
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
                case 'g':
                case 'G':
                    dash.select(model::DashboardSection::Home);
                    bumpNotice();
                    return true;
                case 's':
                case 'S':
                    // On Settings: S cycles shader. Elsewhere: Sessions jump.
                    if (dash.section == model::DashboardSection::Settings) {
                        if (!gfx::fieldEnabled()) gfx::setFieldEnabled(true);
                        else gfx::cycleField(1);
                        dash.notice = std::string("shader · ") + gfx::activeFieldName();
                        return true;
                    }
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
                case '?':
                    dash.select(model::DashboardSection::Settings);
                    bumpNotice();
                    return true;
                case 'b':
                case 'B':  // toggle field shader on/off (global)
                    gfx::toggleFieldEnabled();
                    dash.notice = gfx::fieldEnabled()
                                      ? std::string("shader · ") + gfx::activeFieldName()
                                      : std::string("shader · off");
                    return true;
                case 'n':
                case 'N':
                    if (dash.section == model::DashboardSection::Sessions ||
                        dash.section == model::DashboardSection::Home)
                        createSession();
                    return true;
                case 'd':
                case 'D':
                    if (dash.section == model::DashboardSection::Sessions) deleteSelectedSession();
                    return true;
                case 'y':
                case 'Y':
                    if (!dash.yankBuffer.empty()) {
                        dash.notice = "yank: " + dash.yankBuffer;
                    }
                    return true;
                case 'R':
                    dash.refreshAll();
                    dash.notice = "refreshed";
                    return true;
                case 'T':
                    theme::toggle();
                    dash.notice = std::string("theme · ") + theme::name();
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
        const float tsec = gfx::nowSeconds();
        const int tvar = gfx::themeVariantIndex();

        // Field bg (or solid theme base when disabled). No stage ─ chrome.
        gfx::drawFieldBg(surface, page, tvar, tsec);

        drawAppBar(surface, page, tier);

        const int dockReserve = 4;
        int stageTop = page.y + 3;  // page.y+2 = pure field/theme strip
        int stageH = std::max(6, page.bottom() - dockReserve - stageTop);
        inkcell::Rect stage{page.x, stageTop, page.w, stageH};
        gfx::drawBorderlessPanel(surface, stage, theme::panel_bg());

        inkcell::Rect content{stage.x + 2, stage.y + 1, std::max(1, stage.w - 4),
                              std::max(1, stage.h - 2)};
        const int maxSlide = std::max(4, std::min(12, content.h / 3));
        int yOff = dash.pageSlideRows(maxSlide);
        if (yOff != 0) {
            inkcell::Rect slid = content;
            slid.y += yOff;
            slid.h = std::max(1, content.h - yOff);
            drawContent(surface, slid);
        } else {
            drawContent(surface, content);
        }

        static const std::vector<components::PillItem> pills = {
            {"g", "Home"},
            {"s", "Sessions"},
            {"a", "Manifests"},
            {"?", "Settings"},
        };
        components::drawPillDock(surface, page.x, page.w, page.bottom() - 1, pills,
                                 dash.navigationIndex, dash.navPrevIndex, dash.navAnimT(),
                                 dash.focus == model::DashboardFocus::Dock);

        // Command palette above everything
        components::drawCmdPalette(surface, page, model_->cmdPalette);
    }

   private:
    void runPaletteAction(const std::string& id) {
        auto& dash = model_->dashboard;
        if (id == "nav.home") dash.select(model::DashboardSection::Home);
        else if (id == "nav.sessions") dash.select(model::DashboardSection::Sessions);
        else if (id == "nav.manifests") {
            dash.select(model::DashboardSection::Manifests);
            dash.refreshManifests();
        } else if (id == "nav.help" || id == "nav.settings")
            dash.select(model::DashboardSection::Settings);
        else if (id == "nav.chat") model_->pendingRoute = "agent";
        else if (id == "act.refresh") {
            dash.refreshAll();
            dash.notice = "refreshed";
        } else if (id == "act.theme") theme::toggle();
        else if (id == "act.shader") gfx::cycleField(1);
        else if (id == "act.shader_off") gfx::setFieldEnabled(false);
        else if (id == "act.shader_on") gfx::setFieldEnabled(true);
        else if (id == "act.launch") activate();
        else if (id == "sys.quit") model_->pendingRoute = "quit";
        bumpNotice();
    }
    std::string activeName() const {
        return !model_->agentName.empty() ? model_->agentName : nonempty(cfg_.agentName, "builtin");
    }
    std::string activeManifest() const {
        return !model_->activeManifestPath.empty() ? model_->activeManifestPath : cfg_.manifestPath;
    }

    void drawAppBar(inkcell::Surface& surface, inkcell::Rect page, layout::DensityTier tier) const {
        auto bar = theme::panel_2();
        // Two rows only — no hairline under bar; wallpaper/ripple owns the gap.
        surface.fill({page.x, page.y, page.w, 2}, " ", bar);

        // Brand: CORTEX bold + MK3 cyan — contiguous "CORTEX MK3" for scan/tests
        auto brand = theme::bright().with_bg(bar.bg);
        auto mk = theme::cyan().with_bg(bar.bg);
        surface.text({page.x, page.y}, "CORTEX ", brand);
        surface.text({page.x + 7, page.y}, "MK3", mk);
        auto sec = theme::italic_accent().with_bg(bar.bg);
        surface.text({page.x + 10, page.y},
                     "  ·  " + std::string(model::dashboardSectionName(model_->dashboard.section)),
                     sec);

        std::string right = activeName() + "  ·  " + nonempty(cfg_.provider, model_->agentProvider) +
                            "/" + nonempty(cfg_.model, model_->agentModel) + "  ·  " +
                            layout::densityName(tier);
        // Prefer live identity
        right = activeName() + "  ·  " + nonempty(model_->agentProvider, nonempty(cfg_.provider, "?")) +
                "/" + nonempty(model_->agentModel, nonempty(cfg_.model, "?")) + "  ·  " +
                layout::densityName(tier);
        int rw = inkcell::text::display_width(right);
        surface.text({std::max(page.x, page.right() - rw), page.y}, right,
                     theme::dim().with_bg(bar.bg));

        std::string sub;
        if (!model_->launchError.empty()) {
            sub = "error  " + model_->launchError;
        } else if (model_->dashboard.searchMode || !model_->dashboard.searchQuery.empty()) {
            sub = "/" + model_->dashboard.searchQuery +
                  (model_->dashboard.searchMode ? "█" : "") + "   esc clears";
        } else if (!model_->dashboard.notice.empty()) {
            sub = model_->dashboard.notice;
        } else {
            sub = "session " + suffix(model_->activeSessionId) + "   registry " +
                  std::to_string(model_->dashboard.manifests.size()) + "   " +
                  (model_->running ? "● live" : "○ idle");
        }
        surface.text({page.x, page.y + 1}, inkcell::text::truncate(sub, page.w),
                     (!model_->launchError.empty() ? theme::red() : theme::dim()).with_bg(bar.bg));
    }

    void highlightActiveAgent() {
        const std::string am = activeManifest();
        const std::string an = activeName();
        if (am.empty() && an.empty()) return;
        for (int i = 0; i < static_cast<int>(model_->dashboard.manifests.size()); ++i) {
            const auto& m = model_->dashboard.manifests[static_cast<size_t>(i)];
            if (m.kind != "agent") continue;
            if ((!an.empty() && m.name == an) || (!am.empty() && m.path == am)) {
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
            dash.notice.clear();  // Home/Help — app bar pulse is enough
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
            case model::DashboardSection::Home: drawHome(surface, frame); break;
            case model::DashboardSection::Sessions: drawSessions(surface, frame); break;
            case model::DashboardSection::Manifests: drawManifests(surface, frame); break;
            case model::DashboardSection::Settings: drawSettings(surface, frame); break;
        }
    }

    void sectionHead(inkcell::Surface& surface, inkcell::Rect frame, const std::string& title,
                     const std::string& subtitle) const {
        // No ─ rules — title + italic subtitle only; field/theme is the chrome.
        surface.text({frame.x, frame.y}, title, theme::bright());
        if (!subtitle.empty())
            surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(subtitle, frame.w),
                         theme::italic_dim());
    }

    void metricTile(inkcell::Surface& surface, inkcell::Rect r, const std::string& label,
                    const std::string& value, inkcell::Style valueSt) const {
        // Borderless tile — no ─ box chrome
        surface.fill(r, " ", theme::panel_2());
        surface.text({r.x + 2, r.y + 1}, inkcell::text::truncate(label, r.w - 4), theme::dim());
        surface.text({r.x + 2, r.y + 2}, inkcell::text::truncate(value, r.w - 4), valueSt);
    }

    void drawHome(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionHead(surface, frame, "Home",
                    "live agent · harness · runtime  ·  ctrl-j/k dock  ·  a registry");
        int y = frame.y + 4;

        int tileW = std::max(16, (frame.w - 6) / 4);
        int tileH = 4;
        if (y + tileH < frame.bottom() && frame.w >= 64) {
            metricTile(surface, {frame.x, y, tileW, tileH}, "AGENT", activeName(), theme::bright());
            metricTile(surface, {frame.x + tileW + 2, y, tileW, tileH}, "STATUS",
                       model_->running ? "running" : "ready",
                       model_->running ? theme::green() : theme::text());
            metricTile(surface, {frame.x + 2 * (tileW + 2), y, tileW, tileH}, "REGISTRY",
                       std::to_string(model_->dashboard.manifests.size()), theme::cyan());
            metricTile(surface, {frame.x + 3 * (tileW + 2), y, tileW, tileH}, "SESSIONS",
                       std::to_string(model_->dashboard.sessions.size()), theme::text());
            y += tileH + 2;
        }

        surface.text({frame.x, y++}, "RUNTIME", theme::cyan_soft());
        components::fieldLine(surface, frame.x, y++, frame.w, "engine",
                              nonempty(model_->agentProvider, nonempty(cfg_.provider, "?")) + "/" +
                                  nonempty(model_->agentModel, nonempty(cfg_.model, "?")));
        components::fieldLine(surface, frame.x, y++, frame.w, "manifest",
                              activeManifest().empty() ? "none" : activeManifest());
        components::fieldLine(surface, frame.x, y++, frame.w, "session",
                              suffix(model_->activeSessionId));
        components::fieldLine(surface, frame.x, y++, frame.w, "turn",
                              model_->running ? "running" : "idle");
        y += 1;

        surface.text({frame.x, y++}, "HARNESS", theme::amber_soft());
        components::fieldLine(surface, frame.x, y++, frame.w, "harness", basename(cfg_.harnessPath));
        components::fieldLine(surface, frame.x, y++, frame.w, "system",
                              basename(cfg_.systemPromptPath));
        components::fieldLine(surface, frame.x, y++, frame.w, "persona", basename(cfg_.personaPath));
        if (model_->rootAgent) {
            y += 1;
            auto oneLine = [&](const char* lab, const std::vector<std::string>& names) {
                if (y >= frame.bottom()) return;
                std::string joined;
                for (const auto& n : names) {
                    if (!joined.empty()) joined += " · ";
                    joined += n;
                }
                components::fieldLine(surface, frame.x, y++, frame.w, lab,
                                      joined.empty() ? "none" : joined);
            };
            oneLine("tools", model_->rootAgent->toolNames());
            oneLine("agents", model_->rootAgent->subAgentNames());
            oneLine("feeds", model_->rootAgent->feedNames());
        }
        if (!model_->launchError.empty() && y < frame.bottom()) {
            y += 1;
            surface.text({frame.x, y},
                         inkcell::text::truncate("launch error: " + model_->launchError, frame.w),
                         theme::red());
        }
    }

    void drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionHead(surface, frame, "Sessions", "enter resume · n new · d delete");
        const auto& dash = model_->dashboard;
        int y = frame.y + 4;
        if (dash.sessions.empty()) {
            surface.text({frame.x, y++}, "No sessions yet.", theme::dim());
            surface.text({frame.x, y}, "n → create and open chat.", theme::text());
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
                    "enter launches agent · 1-9 kind · f cycle · t tag · / search");

        int y = frame.y + 4;

        // Operable kind chips with indices: [1 agent 12] ...
        std::map<std::string, int> kindCounts;
        // Count from unfiltered would need cache; approximate from current view + labels
        auto allForCount = catalog::discoverManifests(dash.manifestDir);
        for (const auto& m : allForCount) kindCounts[m.kind]++;

        std::vector<components::Chip> kindChips;
        kindChips.push_back({"0", "0:all", static_cast<int>(allForCount.size()),
                             dash.manifestFilter.empty()});
        const auto& facets = model::DashboardState::kindFacets();
        for (size_t i = 1; i < facets.size() && i <= 9; ++i) {
            const std::string& k = facets[i];
            int c = kindCounts.count(k) ? kindCounts[k] : 0;
            kindChips.push_back({std::to_string(i), std::to_string(i) + ":" + k, c,
                                 dash.manifestFilter == k});
        }
        int after = y;
        components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, kindChips, &after);
        y = after;

        // Category strip (secondary) — only when useful
        std::map<std::string, int> catCounts;
        for (const auto& m : dash.manifests) catCounts[m.category]++;
        if (catCounts.size() > 1) {
            std::vector<components::Chip> catChips;
            for (const auto& kv : catCounts)
                catChips.push_back({kv.first, kv.first, kv.second, dash.tagFilter == kv.first});
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
            return;
        }

        // Grouped list
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
        int listBottom = frame.bottom();
        // On narrow layouts reserve a floating swipe card strip at the bottom.
        const bool floatingCard = !L.showDetail && frame.h >= 14;
        if (floatingCard) listBottom = frame.bottom() - std::min(12, frame.h / 3);

        int visible = std::max(1, listBottom - y);
        int start = std::max(0, std::min(selRow - visible / 3,
                                         std::max(0, static_cast<int>(rows.size()) - visible)));
        const std::string am = activeManifest();
        const std::string an = activeName();

        const float cardT = dash.cardAnimT();
        const bool swiping = dash.cardAnimating();
        const int nudge = swiping ? components::listNudgeX(dash.cardAnimDir, cardT) : 0;

        for (int ri = start; ri < static_cast<int>(rows.size()) && y < listBottom; ++ri) {
            const auto& row = rows[static_cast<size_t>(ri)];
            if (row.header) {
                surface.text({frame.x, y++}, inkcell::text::truncate(row.text, listW),
                             theme::amber_soft());
                continue;
            }
            const auto& m = dash.manifests[static_cast<size_t>(row.idx)];
            bool selected = row.idx == dash.manifestIndex;
            bool wasPrev = swiping && row.idx == dash.cardPrevIndex;
            bool active =
                m.kind == "agent" && ((!an.empty() && m.name == an) || (!am.empty() && m.path == am));

            int rowX = frame.x + (selected ? nudge : (wasPrev ? -nudge / 2 : 0));
            int rowW = std::max(8, listW - (rowX - frame.x));
            components::drawCardRow(surface, {rowX, y, rowW, 1}, selected, active);
            components::kindChip(surface, rowX + 2, y, m.kind, selected);

            std::string name = std::string(active ? "● " : "  ") + m.name;
            if (m.launchable && m.kind == "agent") name += selected ? "  ↵ launch" : "";
            int nameCol = rowX + 8;
            int nameBudget = rowW - 10;
            if (L.showTagColumn && L.tagColMax > 0) {
                nameBudget = std::max(10, rowW - 10 - L.tagColMax);
                components::drawTagChips(surface, nameCol + nameBudget + 1, y, L.tagColMax, m.tags,
                                         4);
            }
            auto nameSt = selected ? theme::bright() : theme::text();
            if (wasPrev) nameSt = theme::italic_dim();
            surface.text({nameCol, y}, inkcell::text::truncate(name, nameBudget), nameSt);
            ++y;
        }

        // ── Detail / floating card with curved swipe ─────────────────
        inkcell::Rect det;
        if (L.showDetail) {
            det = {frame.x + L.detailX, frame.y + 4, L.detailW, frame.h - 5};
        } else if (floatingCard) {
            int ch = frame.bottom() - listBottom;
            det = {frame.x, listBottom, frame.w, ch};
        } else {
            return;
        }

        // Card well — fill only, no ─ box (field/theme is the frame)
        surface.fill(det, " ", theme::panel_bg());

        auto paintManifestBody = [&](inkcell::Surface& s, inkcell::Rect inner, float alpha,
                                     const catalog::ManifestEntry& m) {
            bool ghost = alpha < 0.55f;
            auto titleSt = ghost ? theme::muted() : theme::bright();
            auto kindSt = ghost ? theme::dim() : theme::kindAccent(m.kind, true);
            auto bodySt = ghost ? theme::italic_dim() : theme::text();
            int dy = inner.y;
            int ix = inner.x;
            int iw = inner.w;
            if (dy >= inner.bottom()) return;

            // Title row: name + version chip
            std::string title = m.name;
            if (!m.version.empty()) title += "  v" + m.version;
            s.text({ix, dy++}, inkcell::text::truncate(title, iw), titleSt);
            if (dy >= inner.bottom()) return;

            // Kind · category · flags
            std::string meta = std::string(assets::kindLabel(m.kind)) + " · " + m.category;
            if (m.nested) meta += " · nested";
            if (m.builtin) meta += " · builtin";
            if (m.launchable) meta += " · launchable";
            s.text({ix, dy++}, inkcell::text::truncate(meta, iw), kindSt);

            if (!m.summary.empty() && dy < inner.bottom() - 10) {
                for (const auto& line : chat::wrapWordsLossless(m.summary, iw)) {
                    if (dy >= inner.bottom() - 10) break;
                    s.text({ix, dy++}, line, bodySt);
                }
            }
            if (dy < inner.bottom()) ++dy;

            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "kind", m.kind);
            if (dy < inner.bottom() && !m.version.empty())
                components::fieldLine(s, ix, dy++, iw, "version", m.version);
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "category", m.category);
            if (dy < inner.bottom() && (!m.provider.empty() || !m.model.empty()))
                components::fieldLine(s, ix, dy++, iw, "engine",
                                      nonempty(m.provider, "?") + "/" + nonempty(m.model, "?"));
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "source", nonempty(m.source, "—"));
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "rel",
                                      m.relPath.empty() ? "—" : m.relPath);
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "path",
                                      m.path.empty() ? "—" : m.path);
            if (dy < inner.bottom()) {
                std::string flags;
                if (m.launchable) flags += "launch ";
                if (m.nested) flags += "nested ";
                if (m.builtin) flags += "builtin ";
                if (flags.empty()) flags = "—";
                components::fieldLine(s, ix, dy++, iw, "flags", flags);
            }

            if (dy < inner.bottom()) {
                ++dy;
                if (dy < inner.bottom()) {
                    auto tagHead = ghost ? theme::dim() : theme::violet_soft();
                    s.text({ix, dy++}, "TAGS", tagHead);
                }
                std::string all;
                for (const auto& t : m.tags) {
                    if (!all.empty()) all += "  ";
                    all += "#" + t;
                }
                auto tagSt = ghost ? theme::italic_dim() : theme::italic();
                tagSt.fg = theme::violet_soft().fg;
                for (const auto& line : chat::wrapWordsLossless(all.empty() ? "—" : all, iw)) {
                    if (dy >= inner.bottom() - 2) break;
                    s.text({ix, dy++}, line, tagSt);
                }
            }
            if (m.launchable && m.kind == "agent" && dy < inner.bottom()) {
                bool isLive =
                    (!am.empty() && m.path == am) || (!an.empty() && m.name == an);
                if (dy < inner.bottom()) ++dy;
                if (dy < inner.bottom())
                    s.text({ix, dy++},
                           isLive ? "LIVE · enter opens chat" : "ENTER LAUNCHES",
                           ghost ? theme::green_soft() : theme::green());
            }
        };

        const auto* sel = dash.selectedManifest();
        if (!sel) return;

        const int cardW = det.w;
        const int cardH = det.h;
        // Rest pose fills the well
        auto restPose = components::CardPose{det.x, det.y, 1.f};

        if (swiping && dash.cardPrevIndex >= 0 &&
            dash.cardPrevIndex < static_cast<int>(dash.manifests.size())) {
            const auto& prev = dash.manifests[static_cast<size_t>(dash.cardPrevIndex)];
            auto outP = components::outgoingPose(det, dash.cardAnimDir, cardT);
            auto inP = components::incomingPose(det, dash.cardAnimDir, cardT);

            // Outgoing first (under), then incoming on top
            components::drawSwipedCard(
                surface, det, outP, cardW, cardH, 0.8f,
                [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                    paintManifestBody(s, inner, a, prev);
                });
            components::drawSwipedCard(
                surface, det, inP, cardW, cardH, 0.15f,
                [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                    paintManifestBody(s, inner, a, *sel);
                });
        } else {
            components::drawSwipedCard(
                surface, det, restPose, cardW, cardH, 0.15f,
                [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                    paintManifestBody(s, inner, a, *sel);
                });
        }
    }

    void drawSettings(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionHead(surface, frame, "Settings", "theme · field shader · keys");
        int y = frame.y + 3;

        // ── Appearance ──────────────────────────────────────────────
        surface.text({frame.x, y++}, "APPEARANCE", theme::violet());

        auto row = [&](const char* key, const std::string& val, const char* bind) {
            if (y >= frame.bottom()) return;
            components::fieldLine(surface, frame.x, y, frame.w - 18, key, val);
            surface.text({frame.x + std::max(20, frame.w - 16), y},
                         bind, theme::italic_accent());
            ++y;
        };

        row("theme", theme::name(), "T");

        std::string shLabel =
            gfx::fieldEnabled() ? std::string(gfx::activeFieldName()) : std::string("off (solid bg)");
        row("shader", shLabel, "S / B");

        if (y < frame.bottom()) {
            surface.text({frame.x, y++}, "",
                         theme::dim());  // spacer via empty
        }
        // Shader catalog list
        if (y < frame.bottom())
            surface.text({frame.x, y++}, "SHADERS", theme::cyan_soft());
        const auto& all = gfx::fieldShaders();
        for (int i = 0; i < static_cast<int>(all.size()) && y < frame.bottom(); ++i) {
            bool on = gfx::fieldEnabled() && i == gfx::activeFieldIndex();
            std::string line = std::string(on ? "▸ " : "  ") + std::to_string(i + 1) + "  " +
                               all[static_cast<size_t>(i)].name;
            surface.text({frame.x, y++}, inkcell::text::truncate(line, frame.w),
                         on ? theme::bright() : theme::muted());
        }
        if (y < frame.bottom()) {
            bool off = !gfx::fieldEnabled();
            surface.text({frame.x, y++},
                         inkcell::text::truncate(std::string(off ? "▸ " : "  ") + "0  off (theme bg)",
                                                 frame.w),
                         off ? theme::amber() : theme::muted());
        }

        y += 1;
        if (y < frame.bottom())
            surface.text({frame.x, y++}, "KEYS", theme::violet());
        const char* keys[] = {
            "  T                 cycle theme (graphite/neon)",
            "  S                 next shader (here in Settings)",
            "  B                 toggle shader on/off (global)",
            "  0                 shader off · 1-7 pick shader",
            "  ctrl-j / ctrl-k   prev / next section",
            "  ctrl-p / spc spc  command palette",
            "  g s a ?           Home / Sessions / Manifests / Settings",
            "  enter             launch selected agent",
            "  c chat · q quit",
        };
        for (const char* line : keys) {
            if (y >= frame.bottom()) break;
            surface.text({frame.x, y++}, line, theme::muted());
        }
    }

    void activate() {
        auto& dash = model_->dashboard;
        if (dash.section == model::DashboardSection::Sessions) {
            resumeSelectedSession();
            return;
        }
        if (dash.section == model::DashboardSection::Home) {
            model_->pendingRoute = "agent";
            return;
        }
        if (dash.section == model::DashboardSection::Manifests) {
            const auto* m = dash.selectedManifest();
            if (!m) {
                dash.notice = "no selection";
                return;
            }
            if (m->kind == "agent" && m->launchable) {
                // Real launch — REPL tick hot-swaps Agent then opens chat.
                dash.yankBuffer = "cortex-mk3 -m " + m->path + " --tui experimental";
                model_->pendingLaunchManifest = m->path;
                dash.notice = "launching " + m->name + "…";
                model_->launchError.clear();
                return;
            }
            dash.notice = m->kind + " · " + m->category + " · inspect only";
            return;
        }
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
            model_->dashboard, sessions, model_->rootAgent->name(),
            nonempty(model_->agentModel, cfg_.model),
            nonempty(model_->agentProvider, cfg_.provider),
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
