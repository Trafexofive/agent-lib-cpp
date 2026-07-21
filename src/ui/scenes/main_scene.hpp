#pragma once
// Dashboard / control hub.
// Cortex-local UI only (src/ui/components + assets). inkcell = primitives.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "base_scene.hpp"
#include "src/session/manager.hpp"
#include "src/ui/assets/glyphs.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/components/chrome.hpp"
#include "src/ui/model/dashboard_controller.hpp"

namespace cortex::mk3::ui::scenes {

class MainScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Dashboard"; }

    void on_enter() override {
        BaseScene::on_enter();
        // Discovery root: explicit --manifest-dir, else walk up from agent.yml,
        // else empty (catalog falls back to cwd/binary/CORTEX_HOME).
        if (!cfg_.manifestDir.empty())
            model_->dashboard.manifestDir = cfg_.manifestDir;
        else if (!cfg_.manifestPath.empty())
            model_->dashboard.manifestDir = cfg_.manifestPath;  // resolveManifestsRoot walks up
        else
            model_->dashboard.manifestDir.clear();
        model_->dashboard.refreshAll();
        if (model_->dashboard.manifests.empty()) {
            model_->dashboard.notice =
                "no PROD manifests found — check manifests/ next to binary or CORTEX_HOME";
        } else {
            model_->dashboard.notice =
                std::to_string(model_->dashboard.manifests.size()) + " manifests · " +
                std::to_string(model_->dashboard.agents.size()) + " top-level agents";
        }
        // Highlight active agent among manifests when possible.
        if (!cfg_.agentName.empty() || !cfg_.manifestPath.empty()) {
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
    }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;
        auto& dash = model_->dashboard;

        if (event.code == KeyCode::Escape) {
            dash.focus = model::DashboardFocus::Navigation;
            return true;
        }
        if (event.code == KeyCode::Tab || event.code == KeyCode::ArrowRight) {
            if (dash.section == model::DashboardSection::Sessions ||
                dash.section == model::DashboardSection::Manifests)
                dash.focus = model::DashboardFocus::Content;
            return true;
        }
        if (event.code == KeyCode::ArrowLeft) {
            dash.focus = model::DashboardFocus::Navigation;
            return true;
        }
        if (event.code == KeyCode::ArrowUp ||
            (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K'))) {
            if (dash.focus == model::DashboardFocus::Content) {
                if (dash.section == model::DashboardSection::Sessions)
                    dash.moveSession(-1);
                else if (dash.section == model::DashboardSection::Manifests)
                    dash.moveManifest(-1);
                else
                    dash.moveNavigation(-1);
            } else {
                dash.moveNavigation(-1);
            }
            return true;
        }
        if (event.code == KeyCode::ArrowDown ||
            (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J'))) {
            if (dash.focus == model::DashboardFocus::Content) {
                if (dash.section == model::DashboardSection::Sessions)
                    dash.moveSession(1);
                else if (dash.section == model::DashboardSection::Manifests)
                    dash.moveManifest(1);
                else
                    dash.moveNavigation(1);
            } else {
                dash.moveNavigation(1);
            }
            return true;
        }
        if (event.code == KeyCode::Enter) {
            activate();
            return true;
        }
        if (event.code == KeyCode::Character) {
            switch (event.ch) {
                case 'c':
                case 'C':
                    model_->pendingRoute = "agent";
                    return true;
                case 'o':
                case 'O':
                    dash.select(model::DashboardSection::Overview);
                    return true;
                case 's':
                case 'S':
                    dash.select(model::DashboardSection::Sessions);
                    dash.focus = model::DashboardFocus::Content;
                    return true;
                case 'a':
                case 'A':
                case 'm':
                case 'M':
                    // m was "return to dashboard from chat" via keymap; here m/a open manifests.
                    dash.select(model::DashboardSection::Manifests);
                    dash.focus = model::DashboardFocus::Content;
                    dash.refreshManifests();
                    return true;
                case 'f':
                case 'F':
                    if (dash.section == model::DashboardSection::Manifests) {
                        dash.cycleManifestFilter();
                        dash.notice = dash.manifestFilter.empty()
                                          ? "filter: all"
                                          : "filter: " + dash.manifestFilter;
                    }
                    return true;
                case 'h':
                case 'H':
                    dash.select(model::DashboardSection::Harness);
                    return true;
                case 'r':
                    dash.select(model::DashboardSection::Runtime);
                    return true;
                case '?':
                    dash.select(model::DashboardSection::Help);
                    return true;
                case 'n':
                case 'N':
                    if (dash.section == model::DashboardSection::Sessions ||
                        dash.section == model::DashboardSection::Overview)
                        createSession();
                    return true;
                case 'd':
                case 'D':
                    if (dash.section == model::DashboardSection::Sessions &&
                        dash.focus == model::DashboardFocus::Content)
                        deleteSelectedSession();
                    return true;
                case 'R':
                    dash.refreshAll();
                    dash.notice = "sessions + manifests refreshed";
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
        if (layout::render_min_size_notice(surface)) return;
        surface.clear(theme::base_bg());
        inkcell::Rect page = layout::page(surface);

        // Header
        components::headerStrip(
            surface, {page.x, page.y, page.w, 2},
            std::string(assets::DASH_TITLE) + "  /  " + assets::DASH_SUB,
            nonempty(cfg_.provider, "?") + "/" + nonempty(cfg_.model, "default") + "  ·  " +
                theme::name());

        int top = page.y + 3;
        int footerY = page.bottom() - 1;
        int navW = page.w >= 110 ? 28 : 22;
        inkcell::Rect nav{page.x, top, navW, std::max(1, footerY - top - 1)};
        inkcell::Rect content{nav.right() + 2, top, std::max(1, page.right() - nav.right() - 2),
                              nav.h};

        drawNavigation(surface, nav);
        drawContent(surface, content);
        drawFooter(surface, {page.x, footerY, page.w, 1});
    }

   private:
    struct NavItem {
        model::DashboardSection section;
        const char* key;
        const char* label;
        const char* blurb;
    };

    static const std::vector<NavItem>& navItems() {
        static const std::vector<NavItem> v = {
            {model::DashboardSection::Overview, "o", "Overview", "workspace pulse"},
            {model::DashboardSection::Sessions, "s", "Sessions", "resume · new · delete"},
            {model::DashboardSection::Manifests, "a", "Manifests", "PROD registry hub"},
            {model::DashboardSection::Harness, "h", "Harness", "live capability surface"},
            {model::DashboardSection::Runtime, "r", "Runtime", "provider · process"},
            {model::DashboardSection::Help, "?", "Help", "keys + hub flow"},
        };
        return v;
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

    void drawNavigation(inkcell::Surface& surface, inkcell::Rect frame) const {
        components::fillRect(surface, frame, theme::panel_bg());
        components::accentBar(surface, frame.x, frame.y, frame.h,
                              model_->dashboard.focus == model::DashboardFocus::Navigation
                                  ? theme::footer_accent_focus()
                                  : theme::footer_accent_idle());

        surface.text({frame.x + 2, frame.y}, "NAV", theme::dim());
        int y = frame.y + 2;
        const auto& items = navItems();
        // Single-row nav entries so 80x24 still shows every section.
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const auto& it = items[static_cast<size_t>(i)];
            bool sel = static_cast<int>(model_->dashboard.section) == i;
            bool focus = sel && model_->dashboard.focus == model::DashboardFocus::Navigation;
            inkcell::Rect row{frame.x + 1, y, frame.w - 2, 1};
            components::fillRect(surface, row, focus ? theme::panel_3() : theme::panel_bg());
            if (focus)
                components::accentBar(surface, row.x, row.y, 1, theme::footer_accent_focus());
            std::string label = std::string(it.key) + "  " + it.label;
            surface.text({row.x + 2, row.y}, inkcell::text::truncate(label, row.w - 3),
                         focus ? theme::bright() : (sel ? theme::text() : theme::dim()));
            ++y;
        }

        if (frame.h >= 6) {
            surface.text({frame.x + 2, frame.bottom() - 3}, "c  chat", theme::green());
            surface.text({frame.x + 2, frame.bottom() - 2}, "T  theme", theme::dim());
            surface.text({frame.x + 2, frame.bottom() - 1}, "q  quit", theme::dim());
        }
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
                     "Hub pulse — chat, sessions, PROD manifests registry");
        int y = frame.y + 5;
        components::fieldLine(surface, frame.x, y++, frame.w, "agent",
                              nonempty(cfg_.agentName, "builtin"));
        components::fieldLine(surface, frame.x, y++, frame.w, "status",
                              model_->running ? "running" : "ready");
        components::fieldLine(surface, frame.x, y++, frame.w, "session",
                              suffix(model_->activeSessionId));
        components::fieldLine(surface, frame.x, y++, frame.w, "manifest",
                              cfg_.manifestPath.empty() ? "builtin" : basename(cfg_.manifestPath));
        y += 1;
        surface.text({frame.x, y++}, "REGISTRY", theme::dim());
        components::fieldLine(surface, frame.x, y++, frame.w, "manifests",
                              std::to_string(model_->dashboard.manifests.size()) + " entries");
        components::fieldLine(surface, frame.x, y++, frame.w, "agents",
                              std::to_string(model_->dashboard.agents.size()) + " launchable");
        components::fieldLine(surface, frame.x, y++, frame.w, "sessions",
                              std::to_string(model_->dashboard.sessions.size()));
        y += 1;
        surface.text({frame.x, y++}, "QUICK", theme::dim());
        surface.text({frame.x, y++}, "  c / Enter   open chat", theme::green());
        surface.text({frame.x, y++}, "  a / m       manifests hub (PROD registry)", theme::text());
        surface.text({frame.x, y++}, "  s           sessions", theme::text());
        surface.text({frame.x, y++}, "  f           cycle kind filter (in Manifests)", theme::text());
        surface.text({frame.x, y++}, "  n           new session → chat", theme::text());
        y += 1;
        surface.text({frame.x, y++}, "LAYOUT RULE", theme::dim());
        surface.text({frame.x, y++}, "  manifests/  = PROD / std registry", theme::text());
        surface.text({frame.x, y}, "  config/     = DEV / MVP / experiments", theme::dim());
    }

    void drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Sessions",
                     "Enter resume · n new · d delete · R refresh");
        const auto& dash = model_->dashboard;
        int y = frame.y + 5;
        if (dash.sessions.empty()) {
            surface.text({frame.x, y++}, "No saved sessions.", theme::dim());
            surface.text({frame.x, y}, "Press n for a clean session → chat.", theme::text());
            return;
        }
        int visible = std::max(1, frame.bottom() - y - 2);
        int start = std::max(0, std::min(dash.sessionIndex - visible / 2,
                                         static_cast<int>(dash.sessions.size()) - visible));
        for (int i = start; i < static_cast<int>(dash.sessions.size()) && y < frame.bottom(); ++i) {
            const auto& s = dash.sessions[static_cast<size_t>(i)];
            bool selected =
                i == dash.sessionIndex && dash.focus == model::DashboardFocus::Content;
            std::string line = suffix(s.id) + "  " + nonempty(s.agentName, "?") + "  " +
                               std::to_string(s.turnCount) + "r  " + s.updated;
            components::listRow(surface, {frame.x, y, frame.w, 1}, line, selected);
            ++y;
        }
    }

    void drawManifests(inkcell::Surface& surface, inkcell::Rect frame) const {
        const auto& dash = model_->dashboard;
        std::string filter = dash.manifestFilter.empty() ? "all" : dash.manifestFilter;
        sectionTitle(surface, frame, "Manifests",
                     std::string("PROD registry · filter=") + filter +
                         " · f cycle · R refresh · " +
                         std::to_string(dash.manifests.size()) + " entries");

        int y = frame.y + 5;
        if (dash.manifests.empty()) {
            surface.text({frame.x, y++}, assets::MANIFESTS_EMPTY, theme::amber());
            surface.text({frame.x, y++}, assets::MANIFESTS_HINT, theme::text());
            y += 1;
            surface.text({frame.x, y++}, "Searched roots:", theme::dim());
            auto roots = catalog::manifestsSearchRoots(dash.manifestDir);
            if (roots.empty()) {
                surface.text({frame.x, y++}, "  (none) — set CORTEX_HOME or run from repo root", theme::red());
            } else {
                for (const auto& r : roots) {
                    if (y >= frame.bottom()) break;
                    surface.text({frame.x, y++},
                                 inkcell::text::truncate("  [" + r.second + "] " + r.first, frame.w),
                                 theme::dim());
                }
            }
            surface.text({frame.x, std::min(y + 1, frame.bottom() - 1)},
                         "R refresh · override: --manifest-dir path/to/manifests", theme::green());
            return;
        }

        // List + detail split when wide enough.
        int listW = frame.w >= 90 ? frame.w * 45 / 100 : frame.w;
        int detailX = frame.x + listW + 2;
        int detailW = frame.w >= 90 ? frame.w - listW - 2 : 0;

        int listBottom = frame.bottom() - (detailW > 0 ? 0 : 4);
        int visible = std::max(1, listBottom - y);
        int start = std::max(0, std::min(dash.manifestIndex - visible / 2,
                                         static_cast<int>(dash.manifests.size()) - visible));

        for (int i = start; i < static_cast<int>(dash.manifests.size()) && y < listBottom; ++i) {
            const auto& m = dash.manifests[static_cast<size_t>(i)];
            bool selected =
                i == dash.manifestIndex && dash.focus == model::DashboardFocus::Content;
            bool active = m.kind == "agent" &&
                          ((!cfg_.agentName.empty() && m.name == cfg_.agentName) ||
                           (!cfg_.manifestPath.empty() && m.path == cfg_.manifestPath));

            inkcell::Rect row{frame.x, y, listW, 1};
            components::fillRect(surface, row, selected ? theme::panel_3() : theme::panel_bg());
            if (selected)
                components::accentBar(surface, row.x, row.y, 1, theme::footer_accent_focus());

            components::kindChip(surface, row.x + (selected ? 2 : 1), row.y, m.kind, selected);
            std::string name = (active ? std::string(assets::ACTIVE) + " " : std::string("  ")) +
                               m.name;
            if (!m.version.empty()) name += "  v" + m.version;
            surface.text({row.x + 7, row.y},
                         inkcell::text::truncate(name, std::max(1, listW - 8)),
                         selected ? theme::bright() : theme::text());
            ++y;
        }

        const catalog::ManifestEntry* sel = dash.selectedManifest();
        if (!sel) return;

        if (detailW > 0) {
            // Side detail panel
            inkcell::Rect det{detailX, frame.y + 5, detailW, frame.h - 6};
            components::fillRect(surface, det, theme::panel_2());
            components::accentBar(surface, det.x, det.y, det.h, theme::footer_accent_idle());
            int dy = det.y + 1;
            surface.text({det.x + 2, dy++},
                         inkcell::text::truncate(std::string(assets::kindLabel(sel->kind)) +
                                                     "  ·  " + sel->name,
                                                 det.w - 3),
                         theme::bright());
            if (!sel->summary.empty())
                surface.text({det.x + 2, dy++},
                             inkcell::text::truncate(sel->summary, det.w - 3), theme::text());
            dy += 1;
            components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "kind", sel->kind);
            if (!sel->version.empty())
                components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "version",
                                      sel->version);
            if (!sel->provider.empty() || !sel->model.empty())
                components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "engine",
                                      nonempty(sel->provider, "?") + "/" +
                                          nonempty(sel->model, "?"));
            components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "source", sel->source);
            components::fieldLine(surface, det.x + 2, dy++, det.w - 3, "rel",
                                  sel->relPath.empty() ? sel->path : sel->relPath);
            dy += 1;
            if (sel->launchable) {
                surface.text({det.x + 2, dy++}, "LAUNCHABLE", theme::green());
                surface.text({det.x + 2, dy++},
                             inkcell::text::truncate(
                                 "cortex-mk3 -m " + sel->name + " --tui experimental", det.w - 3),
                             theme::green());
                surface.text({det.x + 2, dy}, "Enter copies launch hint to notice bar",
                             theme::dim());
            } else {
                surface.text({det.x + 2, dy++}, "INSPECT ONLY", theme::amber());
                surface.text({det.x + 2, dy},
                             "Runtime renderers land per-kind (workflow next).", theme::dim());
            }
        } else {
            // Narrow: detail under list
            y = std::min(y + 1, frame.bottom() - 3);
            surface.text({frame.x, y++},
                         inkcell::text::truncate(std::string(assets::kindTag(sel->kind)) + "  " +
                                                     sel->name + "  " + sel->summary,
                                                 frame.w),
                         theme::text());
            if (sel->launchable)
                surface.text({frame.x, frame.bottom() - 1},
                             inkcell::text::truncate(
                                 "cortex-mk3 -m " + sel->name + " --tui experimental", frame.w),
                             theme::green());
            else
                surface.text({frame.x, frame.bottom() - 1},
                             inkcell::text::truncate(sel->relPath, frame.w), theme::dim());
        }
    }

    void drawHarness(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Harness",
                     "Active prompt stack + imported capability surface");
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
            surface.text({frame.x, y++},
                         label + "  (" + std::to_string(names.size()) + ")", theme::dim());
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
        sectionTitle(surface, frame, "Runtime", "Process + provider for this instance");
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
        components::fieldLine(surface, frame.x, y++, frame.w, "pending",
                              std::to_string(model_->pendingOps));
        components::fieldLine(surface, frame.x, y++, frame.w, "actions",
                              std::to_string(model_->actionCount));
        components::fieldLine(surface, frame.x, y++, frame.w, "results",
                              std::to_string(model_->resultCount));
        y += 1;
        surface.text({frame.x, y++}, "Pick an agent via Manifests, then relaunch:", theme::dim());
        surface.text({frame.x, y}, "  cortex-mk3 -m <name> --tui experimental", theme::green());
    }

    void drawHelp(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Help", "Hub navigation");
        int y = frame.y + 5;
        const char* lines[] = {
            "j/k  arrows     move nav / list",
            "Tab / Right     focus content (Sessions, Manifests)",
            "Left / Esc      back to navigation",
            "Enter           activate (chat / resume / launch hint)",
            "c               open chat",
            "a / m           Manifests hub (PROD registry)",
            "f               cycle kind filter (agent/tool/feed/...)",
            "s               Sessions",
            "h r ?           Harness · Runtime · Help",
            "n               new session → chat",
            "d               delete session",
            "R               refresh sessions + manifests",
            "T               theme graphite ↔ neon",
            "q               quit",
            "",
            "manifests/ = PROD std registry (recursive scan)",
            "config/    = DEV / MVP experiments (not auto-hubbed)",
            "UI chrome  = src/ui/{assets,components} — not inkcell",
        };
        for (const char* line : lines) {
            if (y >= frame.bottom()) break;
            surface.text({frame.x, y++}, line, line[0] == '\0' ? theme::dim() : theme::text());
        }
    }

    void drawFooter(inkcell::Surface& surface, inkcell::Rect row) const {
        std::string left = model_->dashboard.notice;
        std::string right;
        if (model_->dashboard.focus == model::DashboardFocus::Content) {
            if (model_->dashboard.section == model::DashboardSection::Sessions)
                right = "j/k · Enter resume · n new · d del";
            else if (model_->dashboard.section == model::DashboardSection::Manifests)
                right = "j/k · f filter · Enter · R refresh";
            else
                right = "j/k · Enter · Esc";
        } else {
            right = "j/k nav · c chat · a manifests · q quit";
        }
        if (left.empty()) left = "hub";
        components::footerBar(surface, row, left, right);
    }

    void activate() {
        auto& dash = model_->dashboard;
        if (dash.focus == model::DashboardFocus::Content &&
            dash.section == model::DashboardSection::Sessions) {
            resumeSelectedSession();
            return;
        }
        if (dash.focus == model::DashboardFocus::Content &&
            dash.section == model::DashboardSection::Manifests) {
            if (const auto* m = dash.selectedManifest()) {
                if (m->launchable) {
                    dash.notice = "launch: cortex-mk3 -m " + m->name + " --tui experimental";
                } else {
                    dash.notice = m->kind + ": " + m->relPath + " (inspect — renderer TBD)";
                }
            } else {
                dash.notice = "no manifest selected";
            }
            return;
        }
        switch (dash.section) {
            case model::DashboardSection::Overview:
                model_->pendingRoute = "agent";
                break;
            case model::DashboardSection::Sessions:
            case model::DashboardSection::Manifests:
                dash.focus = model::DashboardFocus::Content;
                break;
            default:
                break;
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
        if (action.is("scroll.up"))
            model_->dashboard.moveNavigation(-1);
        else if (action.is("scroll.down"))
            model_->dashboard.moveNavigation(1);
    }
};

}  // namespace cortex::mk3::ui::scenes
