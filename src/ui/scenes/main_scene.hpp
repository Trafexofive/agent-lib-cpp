#pragma once
// Final dashboard/control surface. Real actions only: chat, session lifecycle,
// harness inventory, runtime inspection, help, quit.

#include <algorithm>
#include <chrono>
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
        model_->dashboard.refreshSessions();
    }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;
        auto& dash = model_->dashboard;

        if (event.code == KeyCode::Escape) {
            dash.focus = model::DashboardFocus::Navigation;
            return true;
        }
        if (event.code == KeyCode::Tab || event.code == KeyCode::ArrowRight) {
            if (dash.section == model::DashboardSection::Sessions)
                dash.focus = model::DashboardFocus::Content;
            return true;
        }
        if (event.code == KeyCode::ArrowLeft) {
            dash.focus = model::DashboardFocus::Navigation;
            return true;
        }
        if (event.code == KeyCode::ArrowUp ||
            (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K'))) {
            if (dash.focus == model::DashboardFocus::Content &&
                dash.section == model::DashboardSection::Sessions)
                dash.moveSession(-1);
            else
                dash.moveNavigation(-1);
            return true;
        }
        if (event.code == KeyCode::ArrowDown ||
            (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J'))) {
            if (dash.focus == model::DashboardFocus::Content &&
                dash.section == model::DashboardSection::Sessions)
                dash.moveSession(1);
            else
                dash.moveNavigation(1);
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
                case 's': case 'S': dash.select(model::DashboardSection::Sessions); dash.focus = model::DashboardFocus::Content; return true;
                case 'h': case 'H': dash.select(model::DashboardSection::Harness); return true;
                case 'r': dash.select(model::DashboardSection::Runtime); return true;
                case '?': dash.select(model::DashboardSection::Help); return true;
                case 'n': case 'N':
                    if (dash.section == model::DashboardSection::Sessions) createSession();
                    return true;
                case 'R': dash.refreshSessions(); dash.notice = "session index refreshed"; return true;
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
        int navWidth = page.w >= 110 ? 29 : 24;
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
            {model::DashboardSection::Overview, "o", "Overview", "active workspace"},
            {model::DashboardSection::Sessions, "s", "Sessions", "resume or start clean"},
            {model::DashboardSection::Harness, "h", "Harness", "prompts and capabilities"},
            {model::DashboardSection::Runtime, "r", "Runtime", "provider and process state"},
            {model::DashboardSection::Help, "?", "Help", "dashboard controls"},
        };
        return value;
    }

    static std::string suffix(const std::string& value) {
        if (value.empty()) return "none";
        return value.size() > 10 ? value.substr(value.size() - 10) : value;
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
                              (cfg_.ephemeral ? "ephemeral" : "persistent");
        surface.text({page.x, page.y + 1}, inkcell::text::truncate(context, page.w), theme::dim());
    }

    void drawNavigation(inkcell::Surface& surface, inkcell::Rect frame) const {
        const auto& dash = model_->dashboard;
        surface.text({frame.x, frame.y}, "WORKSPACE", theme::dim());
        int y = frame.y + 2;
        for (const auto& item : items()) {
            bool selected = item.section == dash.section;
            std::string row = std::string(item.key) + "  " + item.label;
            layout::selected_row(surface, {frame.x, y, frame.w, 1}, row,
                                 selected && dash.focus == model::DashboardFocus::Navigation);
            surface.text({frame.x + 4, y + 1},
                         inkcell::text::truncate(item.description, std::max(1, frame.w - 5)),
                         selected ? theme::text() : theme::dim());
            y += 3;
        }
        if (y + 2 < frame.bottom()) {
            surface.text({frame.x, frame.bottom() - 3}, "c  Open chat", theme::green());
            surface.text({frame.x, frame.bottom() - 2}, "T  Switch theme", theme::dim());
            surface.text({frame.x, frame.bottom() - 1}, "q  Quit", theme::dim());
        }
    }

    void drawContent(inkcell::Surface& surface, inkcell::Rect frame) const {
        switch (model_->dashboard.section) {
            case model::DashboardSection::Overview: drawOverview(surface, frame); break;
            case model::DashboardSection::Sessions: drawSessions(surface, frame); break;
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
        sectionTitle(surface, frame, "Overview", "Current agent workspace and operational state");
        int y = frame.y + 5;
        field(surface, frame.x, y++, frame.w, "agent", nonempty(cfg_.agentName, "builtin"), theme::bright());
        field(surface, frame.x, y++, frame.w, "status", model_->running ? "running" : "ready",
              model_->running ? theme::green() : theme::text());
        // Live operational metrics line. While a turn is running, ticks every
        // frame (elapsed MM:SS + action/result/pending counts + token bytes).
        // When idle, shows an em-dash so the row reserves space and the
        // dashboard layout stays stable.
        auto fmtElapsed = [](int64_t ms) {
            int s = static_cast<int>(ms / 1000);
            int m = s / 60; s %= 60;
            char b[8]; std::snprintf(b, sizeof(b), "%02d:%02d", m, s);
            return std::string(b);
        };
        std::string liveLine;
        if (model_->running && model_->turnStartMs > 0) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            liveLine = fmtElapsed(now - model_->turnStartMs) + " \xc2\xb7 " +
                       std::to_string(model_->actionCount) + " actions \xc2\xb7 " +
                       std::to_string(model_->resultCount) + " results \xc2\xb7 " +
                       std::to_string(model_->pendingOps) + " pending \xc2\xb7 " +
                       std::to_string(model_->tokenBytes) + "b";
        } else {
            liveLine = "\xe2\x80\x94";  // em-dash
        }
        field(surface, frame.x, y++, frame.w, "live", liveLine,
              model_->running ? theme::green() : theme::dim());
        field(surface, frame.x, y++, frame.w, "session", suffix(model_->activeSessionId));
        field(surface, frame.x, y++, frame.w, "manifest", cfg_.manifestPath.empty() ? "builtin surface" : cfg_.manifestPath);
        y += 2;
        surface.text({frame.x, y++}, "CAPABILITIES", theme::dim());
        field(surface, frame.x, y++, frame.w, "tools", std::to_string(cfg_.toolCount));
        field(surface, frame.x, y++, frame.w, "sub-agents", std::to_string(cfg_.subAgentCount));
        field(surface, frame.x, y++, frame.w, "feeds/relics", std::to_string(cfg_.feedCount) + " / " + std::to_string(cfg_.relicCount));
        y += 2;
        surface.text({frame.x, y++}, "c / Enter  open chat", theme::green());
        surface.text({frame.x, y++}, "s          manage sessions", theme::dim());
    }

    void drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Sessions", "Enter resumes selection · n starts clean · R refreshes");
        const auto& dash = model_->dashboard;
        int y = frame.y + 5;
        if (dash.sessions.empty()) {
            surface.text({frame.x, y++}, "No saved sessions.", theme::dim());
            surface.text({frame.x, y}, "Press n to create a clean session.", theme::text());
            return;
        }
        int visible = std::max(1, frame.bottom() - y - 2);
        int start = std::max(0, std::min(dash.sessionIndex - visible / 2,
                                         static_cast<int>(dash.sessions.size()) - visible));
        for (int i = start; i < static_cast<int>(dash.sessions.size()) && y < frame.bottom() - 1; ++i) {
            const auto& info = dash.sessions[static_cast<size_t>(i)];
            bool selected = i == dash.sessionIndex;
            std::string line = suffix(info.id) + "  " +
                               nonempty(info.agentName, "agent") + "  " +
                               std::to_string(info.turnCount) + " records";
            layout::selected_row(surface, {frame.x, y, frame.w, 1}, line,
                                 selected && dash.focus == model::DashboardFocus::Content);
            if (selected && y + 1 < frame.bottom())
                surface.text({frame.x + 3, ++y},
                             inkcell::text::truncate(info.updated + "  " + info.model, frame.w - 3),
                             theme::dim());
            ++y;
        }
    }

    void drawHarness(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Harness", "Active prompt stack and imported capability surface");
        int y = frame.y + 5;
        field(surface, frame.x, y++, frame.w, "manifest", cfg_.manifestPath.empty() ? "none" : cfg_.manifestPath);
        field(surface, frame.x, y++, frame.w, "harness", basename(cfg_.harnessPath));
        field(surface, frame.x, y++, frame.w, "system", basename(cfg_.systemPromptPath));
        field(surface, frame.x, y++, frame.w, "persona", basename(cfg_.personaPath));
        y += 2;
        if (!model_->rootAgent) return;
        auto renderNames = [&](const std::string& label, const std::vector<std::string>& names) {
            if (y >= frame.bottom()) return;
            surface.text({frame.x, y++}, label + "  " + std::to_string(names.size()), theme::dim());
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
        renderNames("FEEDS", model_->rootAgent->feedNames());
        renderNames("RELICS", model_->rootAgent->relicNames());
        renderNames("SUB-AGENTS", model_->rootAgent->subAgentNames());
    }

    void drawRuntime(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Runtime", "Current immutable agent backend and process state");
        int y = frame.y + 5;
        field(surface, frame.x, y++, frame.w, "provider", nonempty(cfg_.provider, "unset"));
        field(surface, frame.x, y++, frame.w, "model", nonempty(cfg_.model, "default"));
        field(surface, frame.x, y++, frame.w, "theme", theme::name());
        field(surface, frame.x, y++, frame.w, "mode", cfg_.ephemeral ? "ephemeral" : "persistent");
        field(surface, frame.x, y++, frame.w, "turn", model_->running ? "running" : "idle");
        field(surface, frame.x, y++, frame.w, "pending", std::to_string(model_->pendingOps));
        field(surface, frame.x, y++, frame.w, "actions", std::to_string(model_->actionCount));
        field(surface, frame.x, y++, frame.w, "results", std::to_string(model_->resultCount));
        y += 2;
        surface.text({frame.x, y++}, "Provider/model are fixed for the active Agent instance.", theme::dim());
        surface.text({frame.x, y}, "Use CLI/provider picker before launch to change backend safely.", theme::dim());
    }

    void drawHelp(inkcell::Surface& surface, inkcell::Rect frame) const {
        sectionTitle(surface, frame, "Help", "Dashboard navigation");
        int y = frame.y + 5;
        const std::vector<std::string> lines = {
            "j/k or arrows   move navigation/session selection",
            "Tab / Right     focus session list",
            "Left / Esc      return to navigation",
            "Enter           activate selected item",
            "c               open chat",
            "n               create clean session (Sessions)",
            "R               refresh session index",
            "T               switch graphite/neon theme",
            "m               return here from chat transcript focus",
            "q               quit",
        };
        for (const auto& line : lines) {
            if (y >= frame.bottom()) break;
            surface.text({frame.x, y++}, line, theme::text());
        }
    }

    void drawFooter(inkcell::Surface& surface, inkcell::Rect row) const {
        std::string left = model_->dashboard.notice;
        std::string right = model_->dashboard.focus == model::DashboardFocus::Content
                                ? "j/k select · Enter resume · n new · Esc navigation"
                                : "j/k navigate · Enter open · c chat · ? help · q quit";
        surface.text({row.x, row.y}, inkcell::text::truncate(left, std::max(0, row.w - static_cast<int>(right.size()) - 2)),
                     theme::green());
        surface.text({std::max(row.x, row.right() - static_cast<int>(right.size())), row.y}, right, theme::dim());
    }

    void activate() {
        auto& dash = model_->dashboard;
        if (dash.focus == model::DashboardFocus::Content &&
            dash.section == model::DashboardSection::Sessions) {
            resumeSelectedSession();
            return;
        }
        switch (dash.section) {
            case model::DashboardSection::Overview:
                model_->pendingRoute = "agent";
                break;
            case model::DashboardSection::Sessions:
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

    void handle(const inkcell::Action& action) override {
        if (action.is("scroll.up")) model_->dashboard.moveNavigation(-1);
        else if (action.is("scroll.down")) model_->dashboard.moveNavigation(1);
    }
};

}  // namespace cortex::mk3::ui::scenes
