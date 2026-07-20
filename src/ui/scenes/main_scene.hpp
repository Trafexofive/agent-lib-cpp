#pragma once
// Dashboard / control surface — ROI-first: chat, sessions, agent catalog,
// harness inventory, runtime, help.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "base_scene.hpp"
#include "src/session/manager.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/model/dashboard_controller.hpp"

namespace cortex::mk3::ui::scenes {

class MainScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Dashboard"; }

    void on_enter() override {
        BaseScene::on_enter();
        model_->dashboard.manifestDir = cfg_.manifestPath;  // discovery still walks globals
        model_->dashboard.refreshAll();
        // Highlight the active agent in the catalog when present.
        if (!cfg_.agentName.empty()) {
            for (int i = 0; i < static_cast<int>(model_->dashboard.agents.size()); ++i) {
                if (model_->dashboard.agents[static_cast<size_t>(i)].name == cfg_.agentName) {
                    model_->dashboard.agentIndex = i;
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
                dash.section == model::DashboardSection::Agents)
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
                if (dash.section == model::DashboardSection::Sessions) dash.moveSession(-1);
                else if (dash.section == model::DashboardSection::Agents) dash.moveAgent(-1);
                else dash.moveNavigation(-1);
            } else {
                dash.moveNavigation(-1);
            }
            return true;
        }
        if (event.code == KeyCode::ArrowDown ||
            (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J'))) {
            if (dash.focus == model::DashboardFocus::Content) {
                if (dash.section == model::DashboardSection::Sessions) dash.moveSession(1);
                else if (dash.section == model::DashboardSection::Agents) dash.moveAgent(1);
                else dash.moveNavigation(1);
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
                case 'c': case 'C': model_->pendingRoute = "agent"; return true;
                case 'o': case 'O': dash.select(model::DashboardSection::Overview); return true;
                case 's': case 'S':
                    dash.select(model::DashboardSection::Sessions);
                    dash.focus = model::DashboardFocus::Content;
                    return true;
                case 'a': case 'A':
                    dash.select(model::DashboardSection::Agents);
                    dash.focus = model::DashboardFocus::Content;
                    dash.refreshAgents();
                    return true;
                case 'h': case 'H': dash.select(model::DashboardSection::Harness); return true;
                case 'r': dash.select(model::DashboardSection::Runtime); return true;
                case '?': dash.select(model::DashboardSection::Help); return true;
                case 'n': case 'N':
                    if (dash.section == model::DashboardSection::Sessions ||
                        dash.section == model::DashboardSection::Overview)
                        createSession();
                    return true;
                case 'd': case 'D':
                    if (dash.section == model::DashboardSection::Sessions &&
                        dash.focus == model::DashboardFocus::Content)
                        deleteSelectedSession();
                    return true;
                case 'R':
                    dash.refreshAll();
                    dash.notice = "sessions + agents refreshed";
                    return true;
                case 'T': theme::toggle(); return true;
                case 'q': case 'Q': model_->pendingRoute = "quit"; return true;
            }
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        using namespace inkcell;
        if (layout::render_min_size_notice(surface)) return;
        surface.clear(theme::base_bg());
        Rect page = layout::page(surface);
        drawHeader(surface, page);

        int top = page.y + 3;
        int footer = page.bottom() - 1;
        int navWidth = page.w >= 110 ? 30 : 24;
        Rect nav{page.x, top, navWidth, std::max(1, footer - top - 1)};
        Rect content{nav.right() + 3, top, std::max(1, page.right() - nav.right() - 3), nav.h};
        drawNavigation(surface, nav);
        drawContent(surface, content);
        drawFooter(surface, {page.x, footer, page.w, 1});
    }

   private:
    struct NavigationItem {
        model::DashboardSection section;
        const char* key;
        const char* label;
        const char* description;
    };

    static const std::vector<NavigationItem>& items() {
        static const std::vector<NavigationItem> value = {
            {model::DashboardSection::Overview, "o", "Overview", "workspace + quick actions"},
            {model::DashboardSection::Sessions, "s", "Sessions", "resume · new · delete"},
            {model::DashboardSection::Agents, "a", "Agents", "catalog · launch hint"},
            {model::DashboardSection::Harness, "h", "Harness", "prompts + capabilities"},
            {model::DashboardSection::Runtime, "r", "Runtime", "provider · process"},
            {model::DashboardSection::Help, "?", "Help", "keys and flow"},
        };
        return value;
    }

    static std::string suffix(const std::string& value) {
        if (value.empty()) return "none";
        return value.size() > 12 ? value.substr(value.size() - 12) : value;
    }

    static std::string basename(const std::string& path) {
        if (path.empty()) return "none";
        size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    void drawHeader(inkcell::Surface& surface, inkcell::Rect page) const {
        std::string left = "CORTEX MK3  /  DASHBOARD";
        std::string right = nonempty(cfg_.provider, "provider?") + "/" +
                            nonempty(cfg_.model, "default") + "  ·  " + theme::name();
        int rightWidth = inkcell::text::display_width(right);
        surface.text({page.x, page.y}, left, theme::bright());
        surface.text({std::max(page.x, page.right() - rightWidth), page.y}, right, theme::dim());
        std::string context = nonempty(cfg_.agentName, "builtin") + "  ·  session " +
                              suffix(model_->activeSessionId) + "  ·  " +
                              (cfg_.noSession ? "no-session" : cfg_.ephemeral ? "ephemeral" : "persistent");
        surface.text({page.x, page.y + 1}, inkcell::text::truncate(context, page.w), theme::dim());
    }

    void drawNavigation(inkcell::Surface& surface, inkcell::Rect frame) const {
        const auto& dash = model_->dashboard;
        surface.text({frame.x, frame.y}, "WORKSPACE", theme::dim());
        // Compact nav when height is tight so all sections still paint at 80x24.
        const int footerReserve = 3;
        const int available = std::max(0, frame.h - 2 - footerReserve);
        const int n = static_cast<int>(items().size());
        const bool compact = available < n * 3;
        const int step = compact ? 1 : 3;
        int y = frame.y + 2;
        for (const auto& item : items()) {
            if (y >= frame.bottom() - footerReserve) break;
            bool selected = item.section == dash.section;
            std::string row = std::string(item.key) + "  " + item.label;
            layout::selected_row(surface, {frame.x, y, frame.w, 1}, row,
                                 selected && dash.focus == model::DashboardFocus::Navigation);
            if (!compact && y + 1 < frame.bottom() - footerReserve) {
                surface.text({frame.x + 4, y + 1},
                             inkcell::text::truncate(item.description, std::max(1, frame.w - 5)),
                             selected ? theme::text() : theme::dim());
            }
            y += step;
        }
        if (frame.h >= footerReserve) {
            surface.text({frame.x, frame.bottom() - 3}, "c  Open chat", theme::green());
            surface.text({frame.x, frame.bottom() - 2}, "T  Theme", theme::dim());
            surface.text({frame.x, frame.bottom() - 1}, "q  Quit", theme::dim());
        }
    }

    void drawContent(inkcell::Surface& surface, inkcell::Rect frame) const {
        switch (model_->dashboard.section) {
            case model::DashboardSection::Overview: drawOverview(surface, frame); break;
            case model::DashboardSection::Sessions: drawSessions(surface, frame); break;
            case model::DashboardSection::Agents: drawAgents(surface, frame); break;
            case model::DashboardSection::Harness: drawHarness(surface, frame); break;
            case model::DashboardSection::Runtime: drawRuntime(surface, frame); break;
            case model::DashboardSection::Help: drawHelp(surface, frame); break;
        }
    }

    void sectionTitle(inkcell::Surface& surface, inkcell::Rect frame,
                      const std::string& title, const std::string& subtitle) const {
        surface.text({frame.x, frame.y}, title, theme::bright());
        surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(subtitle, frame.w), theme::dim());
        surface.hline({frame.x, frame.y + 3}, frame.w, "─", theme::dim());
    }

    void field(inkcell::Surface& surface, int x, int y, int width,
               const std::string& name, const std::string& value,
               inkcell::Style valueStyle = theme::text()) const {
        surface.text({x, y}, inkcell::text::fit_left(name, 12), theme::dim());
        surface.text({x + 13, y}, inkcell::text::truncate(value, std::max(1, width - 13)), valueStyle);
    }

    void drawOverview(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Overview", "Act here — chat, resume, pick agent, inspect harness");
        int y = frame.y + 5;
        field(surface, frame.x, y++, frame.w, "agent", nonempty(cfg_.agentName, "builtin"), theme::bright());
        field(surface, frame.x, y++, frame.w, "status", model_->running ? "running" : "ready",
              model_->running ? theme::green() : theme::text());

        auto fmtElapsed = [](int64_t ms) {
            int s = static_cast<int>(ms / 1000);
            int m = s / 60;
            s %= 60;
            char b[8];
            std::snprintf(b, sizeof(b), "%02d:%02d", m, s);
            return std::string(b);
        };
        const char* liveLabel = model_->running ? "live" : "last";
        std::string liveLine;
        if (model_->running && model_->turnStartMs > 0) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
            liveLine = fmtElapsed(now - model_->turnStartMs) + " · " +
                       std::to_string(model_->actionCount) + " act · " +
                       std::to_string(model_->resultCount) + " res · " +
                       std::to_string(model_->tokenBytes) + "b";
        } else if (!model_->running && model_->lastTurnElapsedMs > 0) {
            liveLine = model_->status + " " + fmtElapsed(model_->lastTurnElapsedMs) + " · " +
                       std::to_string(model_->tokenBytes) + "b";
        } else {
            liveLine = "—";
        }
        field(surface, frame.x, y++, frame.w, liveLabel, liveLine,
              model_->running ? theme::green() : theme::dim());
        if (model_->running) {
            std::string preview = model_->lastResponseBody().empty()
                                      ? std::string("—")
                                      : inkcell::text::truncate(model_->lastResponseBody(),
                                                                std::max(1, frame.w - 2));
            field(surface, frame.x, y++, frame.w, "preview", preview, theme::text());
        }
        field(surface, frame.x, y++, frame.w, "session", suffix(model_->activeSessionId));
        field(surface, frame.x, y++, frame.w, "manifest",
              cfg_.manifestPath.empty() ? "builtin surface" : basename(cfg_.manifestPath));
        y += 1;

        surface.text({frame.x, y++}, "QUICK ACTIONS", theme::dim());
        surface.text({frame.x, y++}, "  c / Enter   open chat with current agent", theme::green());
        surface.text({frame.x, y++}, "  s           sessions — resume or start clean", theme::text());
        surface.text({frame.x, y++}, "  a           agents catalog — pick launch target", theme::text());
        surface.text({frame.x, y++}, "  h           harness — prompts + tools/sub-agents", theme::text());
        surface.text({frame.x, y++}, "  n           new clean session → chat", theme::text());
        y += 1;

        surface.text({frame.x, y++}, "SURFACE", theme::dim());
        field(surface, frame.x, y++, frame.w, "tools", std::to_string(cfg_.toolCount));
        field(surface, frame.x, y++, frame.w, "sub-agents", std::to_string(cfg_.subAgentCount));
        field(surface, frame.x, y++, frame.w, "feeds/relics",
              std::to_string(cfg_.feedCount) + " / " + std::to_string(cfg_.relicCount));
        field(surface, frame.x, y++, frame.w, "catalog",
              std::to_string(model_->dashboard.agents.size()) + " agents · " +
                  std::to_string(model_->dashboard.sessions.size()) + " sessions");

        if (!model_->dashboard.sessions.empty() && y + 2 < frame.bottom()) {
            y += 1;
            const auto& last = model_->dashboard.sessions.front();
            surface.text({frame.x, y++}, "LATEST SESSION", theme::dim());
            surface.text({frame.x, y},
                         inkcell::text::truncate(
                             "  " + suffix(last.id) + "  " + nonempty(last.agentName, "?") + "  " +
                                 std::to_string(last.turnCount) + " rec  " + last.updated,
                             frame.w),
                         theme::text());
        }
    }

    void drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Sessions",
                     "Enter resume · n new · d delete · R refresh · Tab focus list");
        const auto& dash = model_->dashboard;
        int y = frame.y + 5;
        if (dash.sessions.empty()) {
            surface.text({frame.x, y++}, "No saved sessions.", theme::dim());
            surface.text({frame.x, y}, "Press n to create a clean session and open chat.", theme::text());
            return;
        }
        int visible = std::max(1, frame.bottom() - y - 2);
        int start = std::max(0, std::min(dash.sessionIndex - visible / 2,
                                         static_cast<int>(dash.sessions.size()) - visible));
        for (int i = start; i < static_cast<int>(dash.sessions.size()) && y < frame.bottom() - 1; ++i) {
            const auto& info = dash.sessions[static_cast<size_t>(i)];
            bool selected = i == dash.sessionIndex;
            bool active = info.id == model_->activeSessionId;
            std::string mark = active ? "● " : "  ";
            std::string line = mark + suffix(info.id) + "  " + nonempty(info.agentName, "agent") +
                               "  " + std::to_string(info.turnCount) + " rec";
            layout::selected_row(surface, {frame.x, y, frame.w, 1}, line,
                                 selected && dash.focus == model::DashboardFocus::Content);
            if (selected && y + 1 < frame.bottom())
                surface.text({frame.x + 3, ++y},
                             inkcell::text::truncate(info.updated + "  " + info.model, frame.w - 3),
                             theme::dim());
            ++y;
        }
    }

    void drawAgents(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Agents",
                     "Catalog from manifests/ · Enter shows launch · active marked ●");
        const auto& dash = model_->dashboard;
        int y = frame.y + 5;
        if (dash.agents.empty()) {
            surface.text({frame.x, y++}, "No agents discovered.", theme::dim());
            surface.text({frame.x, y++}, "Place agent.yml under manifests/agents/<name>/", theme::text());
            surface.text({frame.x, y}, "or set CORTEX_HOME / --manifest-dir.", theme::text());
            return;
        }
        int visible = std::max(1, frame.bottom() - y - 6);
        int start = std::max(0, std::min(dash.agentIndex - visible / 2,
                                         static_cast<int>(dash.agents.size()) - visible));
        for (int i = start; i < static_cast<int>(dash.agents.size()) && y < frame.bottom() - 5; ++i) {
            const auto& ag = dash.agents[static_cast<size_t>(i)];
            bool selected = i == dash.agentIndex;
            bool active = (!cfg_.agentName.empty() && ag.name == cfg_.agentName) ||
                          (!cfg_.manifestPath.empty() && ag.manifestPath == cfg_.manifestPath);
            std::string mark = active ? "● " : "  ";
            std::string line = mark + ag.name;
            if (!ag.version.empty()) line += "  v" + ag.version;
            layout::selected_row(surface, {frame.x, y, frame.w, 1}, line,
                                 selected && dash.focus == model::DashboardFocus::Content);
            if (selected) {
                if (y + 1 < frame.bottom()) {
                    std::string meta = nonempty(ag.provider, "?") + "/" + nonempty(ag.model, "default") +
                                       "  ·  " + ag.source;
                    surface.text({frame.x + 3, ++y}, inkcell::text::truncate(meta, frame.w - 3),
                                 theme::dim());
                }
                if (y + 1 < frame.bottom() && !ag.summary.empty()) {
                    surface.text({frame.x + 3, ++y},
                                 inkcell::text::truncate(ag.summary, frame.w - 3), theme::text());
                }
                if (y + 1 < frame.bottom()) {
                    surface.text({frame.x + 3, ++y},
                                 inkcell::text::truncate(ag.manifestPath, frame.w - 3), theme::dim());
                }
            }
            ++y;
        }
        if (y + 1 < frame.bottom()) {
            surface.text({frame.x, frame.bottom() - 2},
                         "Hot-swap in-process is not wired — relaunch with the path below.", theme::dim());
            if (const auto* sel = dash.selectedAgent()) {
                surface.text({frame.x, frame.bottom() - 1},
                             inkcell::text::truncate("cortex-mk3 -m " + sel->name + " --tui experimental",
                                                     frame.w),
                             theme::green());
            }
        }
    }

    void drawHarness(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Harness",
                     "Active prompt stack + imported capability surface (full protocol)");
        int y = frame.y + 5;
        field(surface, frame.x, y++, frame.w, "manifest",
              cfg_.manifestPath.empty() ? "none (builtin tools)" : cfg_.manifestPath);
        field(surface, frame.x, y++, frame.w, "harness", basename(cfg_.harnessPath));
        field(surface, frame.x, y++, frame.w, "system", basename(cfg_.systemPromptPath));
        field(surface, frame.x, y++, frame.w, "persona", basename(cfg_.personaPath));
        y += 1;
        surface.text({frame.x, y++}, "Protocol: thought · action · response · result(runtime)", theme::dim());
        surface.text({frame.x, y++}, "Agent ops: prompt · inspect · continue history · Parent vs User",
                     theme::dim());
        y += 1;
        if (!model_->rootAgent) {
            surface.text({frame.x, y}, "No agent runtime bound.", theme::dim());
            return;
        }
        auto renderNames = [&](const std::string& label, const std::vector<std::string>& names) {
            if (y >= frame.bottom()) return;
            surface.text({frame.x, y++}, label + "  (" + std::to_string(names.size()) + ")", theme::dim());
            std::string joined;
            for (const auto& name : names) {
                if (!joined.empty()) joined += " · ";
                joined += name;
            }
            for (const auto& line : chat::wrapWordsLossless(joined.empty() ? "none" : joined, frame.w - 2)) {
                if (y >= frame.bottom()) break;
                surface.text({frame.x + 2, y++}, line, names.empty() ? theme::dim() : theme::text());
            }
            ++y;
        };
        renderNames("TOOLS", model_->rootAgent->toolNames());
        renderNames("SUB-AGENTS", model_->rootAgent->subAgentNames());
        renderNames("FEEDS", model_->rootAgent->feedNames());
        renderNames("RELICS", model_->rootAgent->relicNames());
    }

    void drawRuntime(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Runtime", "Backend + process state for this Agent instance");
        int y = frame.y + 5;
        field(surface, frame.x, y++, frame.w, "provider", nonempty(cfg_.provider, "unset"));
        field(surface, frame.x, y++, frame.w, "model", nonempty(cfg_.model, "default"));
        field(surface, frame.x, y++, frame.w, "theme", theme::name());
        field(surface, frame.x, y++, frame.w, "lifecycle",
              cfg_.noSession ? "no-session" : cfg_.ephemeral ? "ephemeral (exit-on-done)" : "persistent");
        field(surface, frame.x, y++, frame.w, "turn", model_->running ? "running" : "idle");
        field(surface, frame.x, y++, frame.w, "pending", std::to_string(model_->pendingOps));
        field(surface, frame.x, y++, frame.w, "actions", std::to_string(model_->actionCount));
        field(surface, frame.x, y++, frame.w, "results", std::to_string(model_->resultCount));
        field(surface, frame.x, y++, frame.w, "thoughts", model_->showThoughts ? "on" : "off");
        field(surface, frame.x, y++, frame.w, "truncate", model_->truncateBodies ? "on" : "off");
        y += 2;
        surface.text({frame.x, y++}, "Provider/model are fixed for the active Agent instance.", theme::dim());
        surface.text({frame.x, y}, "Pick agent via catalog (a) then relaunch, or use CLI -m.", theme::dim());
    }

    void drawHelp(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Help", "Dashboard navigation and ROI flow");
        int y = frame.y + 5;
        const std::vector<std::string> lines = {
            "j/k or arrows   move nav / session / agent selection",
            "Tab / Right     focus content list (Sessions, Agents)",
            "Left / Esc      return to navigation",
            "Enter           activate (chat / resume / launch hint)",
            "c               open chat now",
            "o s a h r ?     jump Overview Sessions Agents Harness Runtime Help",
            "n               new clean session → chat",
            "d               delete selected session (Sessions)",
            "R               refresh sessions + agent catalog",
            "T               graphite / neon theme",
            "m               return here from chat",
            "q               quit",
            "",
            "Flow: pick agent (a) → relaunch -m name → sessions (s) → chat (c)",
            "Harness (h) shows live tools/sub-agents/feeds from the active agent.",
        };
        for (const auto& line : lines) {
            if (y >= frame.bottom()) break;
            surface.text({frame.x, y++}, line, line.empty() ? theme::dim() : theme::text());
        }
    }

    void drawFooter(inkcell::Surface& surface, inkcell::Rect row) const {
        std::string left = model_->dashboard.notice;
        std::string right;
        if (model_->dashboard.focus == model::DashboardFocus::Content) {
            if (model_->dashboard.section == model::DashboardSection::Sessions)
                right = "j/k · Enter resume · n new · d del · Esc nav";
            else if (model_->dashboard.section == model::DashboardSection::Agents)
                right = "j/k · Enter launch hint · R refresh · Esc nav";
            else
                right = "j/k · Enter · Esc nav";
        } else {
            right = "j/k nav · Enter · c chat · a agents · ? help · q quit";
        }
        surface.text({row.x, row.y},
                     inkcell::text::truncate(left, std::max(0, row.w - static_cast<int>(right.size()) - 2)),
                     theme::green());
        surface.text({std::max(row.x, row.right() - static_cast<int>(right.size())), row.y}, right,
                     theme::dim());
    }

    void activate() {
        auto& dash = model_->dashboard;
        if (dash.focus == model::DashboardFocus::Content &&
            dash.section == model::DashboardSection::Sessions) {
            resumeSelectedSession();
            return;
        }
        if (dash.focus == model::DashboardFocus::Content &&
            dash.section == model::DashboardSection::Agents) {
            if (const auto* ag = dash.selectedAgent()) {
                dash.notice = "launch: cortex-mk3 -m " + ag->name + " --tui experimental";
            } else {
                dash.notice = "no agent selected";
            }
            return;
        }
        switch (dash.section) {
            case model::DashboardSection::Overview:
                model_->pendingRoute = "agent";
                break;
            case model::DashboardSection::Sessions:
            case model::DashboardSection::Agents:
                dash.focus = model::DashboardFocus::Content;
                break;
            case model::DashboardSection::Harness:
            case model::DashboardSection::Runtime:
            case model::DashboardSection::Help:
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
        if (action.is("scroll.up")) model_->dashboard.moveNavigation(-1);
        else if (action.is("scroll.down")) model_->dashboard.moveNavigation(1);
    }
};

}  // namespace cortex::mk3::ui::scenes
