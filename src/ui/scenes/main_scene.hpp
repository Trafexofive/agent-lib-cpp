#pragma once
// Main control page for experimental inkcell app.
// This is the app composition surface: chat entry, sessions, harness/manifest,
// provider/model, and run context. No placeholder route pages.

#include <algorithm>
#include <string>
#include <vector>

#include "base_scene.hpp"
#include "src/session/manager.hpp"

namespace cortex::mk3::ui::scenes {

class MainScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Main"; }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;
        if (event.code == KeyCode::ArrowUp || (event.code == KeyCode::Character && event.ch == 'k')) {
            mainIndex_ = std::max(0, mainIndex_ - 1);
            return true;
        }
        if (event.code == KeyCode::ArrowDown || (event.code == KeyCode::Character && event.ch == 'j')) {
            mainIndex_ = std::min(kOptionCount - 1, mainIndex_ + 1);
            return true;
        }
        if (event.code == KeyCode::Enter) {
            activateSelection();
            return true;
        }
        if (event.code == KeyCode::Character) {
            if (event.ch == '1' || event.ch == 'c' || event.ch == 'C') {
                model_->pendingRoute = "agent";
                return true;
            }
            if (event.ch == 'q' || event.ch == 'Q') {
                model_->pendingRoute = "quit";
                return true;
            }
            if (event.ch >= '1' && event.ch <= '5') {
                mainIndex_ = event.ch - '1';
                activateSelection();
                return true;
            }
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        using namespace inkcell;
        if (layout::render_min_size_notice(surface)) return;
        surface.clear(theme::base_bg());
        Rect p = layout::page(surface);

        drawTop(surface, p);

        Rect body{p.x, p.y + 6, p.w, std::max(1, p.h - 8)};
        if (body.w >= 120) drawWide(surface, body);
        else drawStandard(surface, body);

        std::string right = cfg_.ephemeral ? "ephemeral" : "session:…" + suffix(cfg_.sessionId);
        surface.text({p.x, p.bottom() - 1}, "↑↓/j/k select · Enter open · 1/c chat · q quit", theme::dim());
        surface.text({std::max(p.x, p.right() - static_cast<int>(right.size())), p.bottom() - 1}, right,
                     theme::dim());
    }

   private:
    static constexpr int kOptionCount = 5;
    int mainIndex_ = 0;

    struct Option {
        const char* key;
        const char* title;
        const char* hint;
        const char* route;
    };

    static std::string suffix(const std::string& id) {
        if (id.empty()) return "none";
        return id.size() > 8 ? id.substr(id.size() - 8) : id;
    }

    static std::string basename(const std::string& path) {
        if (path.empty()) return "none";
        size_t pos = path.find_last_of('/');
        return pos == std::string::npos ? path : path.substr(pos + 1);
    }

    static std::string fit(inkcell::Surface& surface, inkcell::Point p, int w, const std::string& text,
                           inkcell::Style style) {
        surface.text(p, inkcell::text::truncate(text, w), style);
        return text;
    }

    void activateSelection() const {
        if (mainIndex_ == 0) model_->pendingRoute = "agent";
        else if (mainIndex_ == 4) model_->pendingRoute = "quit";
        // Sessions / Harness / Provider are management panels on this page for now.
        // Selection moves the right-hand context; no fake route.
    }

    void handle(const inkcell::Action& action) override {
        if (action.is("scroll.up")) mainIndex_ = std::max(0, mainIndex_ - 1);
        else if (action.is("scroll.down")) mainIndex_ = std::min(kOptionCount - 1, mainIndex_ + 1);
    }

    void drawTop(inkcell::Surface& surface, inkcell::Rect p) const {
        surface.text({p.x, p.y}, "CORTEX MK3", theme::cyan());
        surface.text({p.x, p.y + 1}, "agent workbench · sessions · harness · provider · chat", theme::dim());
        int y = p.y + 3;
        layout::chip(surface, {p.x, y}, "provider " + nonempty(cfg_.provider, "?"), theme::dim());
        layout::chip(surface, {p.x + 28, y}, "model " + nonempty(cfg_.model, "default"), theme::dim());
        layout::chip(surface, {p.x + 58, y}, "agent " + nonempty(cfg_.agentName, "builtin"), theme::text());
        std::string mode = cfg_.manifestPath.empty() ? "main menu" : "manifest loaded → chat-ready";
        surface.text({std::max(p.x, p.right() - static_cast<int>(mode.size())), y}, mode, theme::amber());
        layout::section_rule(surface, {p.x, p.y + 5}, p.w, "control board");
    }

    std::vector<Option> options() const {
        return {
            {"1", "Chat / Agent History", cfg_.manifestPath.empty() ? "open builtin chat surface" : "manifest selected: opens directly into chat", "agent"},
            {"2", "Sessions", "recent sessions and resume context", "sessions"},
            {"3", "Harness / Manifest", "active prompt stack, manifest, tools and sub-agents", "harness"},
            {"4", "Provider / Model", "current backend and model context", "provider"},
            {"q", "Quit", "leave the workbench", "quit"},
        };
    }

    void drawOptions(inkcell::Surface& surface, inkcell::Rect r) const {
        layout::flat_panel(surface, r, theme::panel_bg());
        auto opts = options();
        int y = r.y + 1;
        surface.text({r.x + 1, y++}, "main", theme::bright());
        y++;
        for (int i = 0; i < static_cast<int>(opts.size()) && y + 1 < r.bottom(); ++i) {
            bool sel = i == mainIndex_;
            layout::selected_row(surface, {r.x + 1, y, r.w - 2, 1},
                                 std::string(opts[i].key) + "  " + opts[i].title, sel);
            surface.text({r.x + 5, y + 1}, inkcell::text::truncate(opts[i].hint, std::max(0, r.w - 7)),
                         sel ? theme::text() : theme::dim());
            y += 3;
        }
    }

    void drawChatContext(inkcell::Surface& surface, inkcell::Rect r) const {
        layout::flat_panel(surface, r, theme::panel_bg());
        layout::section_rule(surface, {r.x + 1, r.y + 1}, r.w - 2, "chat context");
        int y = r.y + 3;
        surface.text({r.x + 2, y++}, "agent     " + nonempty(cfg_.agentName, "builtin"), theme::text());
        surface.text({r.x + 2, y++}, "session   " + suffix(cfg_.sessionId), theme::dim());
        surface.text({r.x + 2, y++}, "mode      " + std::string(cfg_.ephemeral ? "ephemeral" : "persistent"), theme::dim());
        surface.text({r.x + 2, y++}, "status    " + model_->status, model_->failed ? theme::red() : theme::dim());
        y++;
        surface.text({r.x + 2, y++}, cfg_.manifestPath.empty() ? "No -m specified: builtin chat surface is active."
                                                               : "-m specified: startup drops directly into chat.",
                     cfg_.manifestPath.empty() ? theme::dim() : theme::green());
    }

    void drawHarness(inkcell::Surface& surface, inkcell::Rect r) const {
        layout::flat_panel(surface, r, theme::panel_2());
        layout::section_rule(surface, {r.x + 1, r.y + 1}, r.w - 2, "harness / manifest");
        int y = r.y + 3;
        surface.text({r.x + 2, y++}, "manifest  " + inkcell::text::truncate(cfg_.manifestPath.empty() ? "none" : cfg_.manifestPath, r.w - 13),
                     cfg_.manifestPath.empty() ? theme::dim() : theme::text());
        surface.text({r.x + 2, y++}, "harness   " + inkcell::text::truncate(basename(cfg_.harnessPath), r.w - 13), theme::dim());
        surface.text({r.x + 2, y++}, "system    " + inkcell::text::truncate(basename(cfg_.systemPromptPath), r.w - 13), theme::dim());
        surface.text({r.x + 2, y++}, "persona   " + inkcell::text::truncate(basename(cfg_.personaPath), r.w - 13), theme::dim());
        y++;
        surface.text({r.x + 2, y++}, "tools     " + std::to_string(cfg_.toolCount), theme::text());
        surface.text({r.x + 2, y++}, "feeds     " + std::to_string(cfg_.feedCount), theme::dim());
        surface.text({r.x + 2, y++}, "relics    " + std::to_string(cfg_.relicCount), theme::dim());
        surface.text({r.x + 2, y++}, "agents    " + std::to_string(cfg_.subAgentCount), theme::amber());
    }

    void drawProvider(inkcell::Surface& surface, inkcell::Rect r) const {
        layout::flat_panel(surface, r, theme::panel_2());
        layout::section_rule(surface, {r.x + 1, r.y + 1}, r.w - 2, "provider / model");
        int y = r.y + 3;
        surface.text({r.x + 2, y++}, "provider  " + nonempty(cfg_.provider, "unset"), theme::text());
        surface.text({r.x + 2, y++}, "model     " + nonempty(cfg_.model, "default"), theme::text());
        y++;
        surface.text({r.x + 2, y++}, "No live quota probes from this page.", theme::dim());
        surface.text({r.x + 2, y++}, "Switching UI comes later via command palette.", theme::dim());
    }

    void drawSessions(inkcell::Surface& surface, inkcell::Rect r) const {
        layout::flat_panel(surface, r, theme::panel_2());
        layout::section_rule(surface, {r.x + 1, r.y + 1}, r.w - 2, "recent sessions");
        int y = r.y + 3;
        try {
            session::SessionManager sm;
            auto list = sm.list();
            if (list.empty()) {
                surface.text({r.x + 2, y++}, "No saved sessions yet.", theme::dim());
                surface.text({r.x + 2, y++}, "Start chat to create one.", theme::dim());
                return;
            }
            int shown = 0;
            for (const auto& s : list) {
                if (y >= r.bottom() - 1 || shown >= 6) break;
                std::string line = suffix(s.id) + "  " + std::to_string(s.turnCount) + " turns  " + s.updated;
                surface.text({r.x + 2, y++}, inkcell::text::truncate(line, r.w - 4), shown == 0 ? theme::text() : theme::dim());
                ++shown;
            }
        } catch (...) {
            surface.text({r.x + 2, y++}, "Could not read session index.", theme::red());
        }
    }

    void drawWide(inkcell::Surface& surface, inkcell::Rect body) const {
        int leftW = 34;
        int midW = std::max(34, (body.w - leftW - 4) / 2);
        inkcell::Rect left{body.x, body.y, leftW, body.h};
        inkcell::Rect mid{left.right() + 2, body.y, midW, body.h};
        inkcell::Rect right{mid.right() + 2, body.y, body.right() - mid.right() - 2, body.h};
        drawOptions(surface, left);
        drawChatContext(surface, {mid.x, mid.y, mid.w, std::max(8, mid.h / 2 - 1)});
        drawSessions(surface, {mid.x, mid.y + std::max(9, mid.h / 2 + 1), mid.w, std::max(6, mid.h / 2 - 1)});
        drawHarness(surface, {right.x, right.y, right.w, std::max(10, right.h / 2)});
        drawProvider(surface, {right.x, right.y + std::max(11, right.h / 2 + 1), right.w, std::max(6, right.h / 2 - 1)});
    }

    void drawStandard(inkcell::Surface& surface, inkcell::Rect body) const {
        int leftW = std::min(34, std::max(28, body.w / 3));
        inkcell::Rect left{body.x, body.y, leftW, body.h};
        inkcell::Rect right{left.right() + 2, body.y, body.right() - left.right() - 2, body.h};
        drawOptions(surface, left);
        if (mainIndex_ == 1) drawSessions(surface, right);
        else if (mainIndex_ == 2) drawHarness(surface, right);
        else if (mainIndex_ == 3) drawProvider(surface, right);
        else drawChatContext(surface, right);
    }
};

}  // namespace cortex::mk3::ui::scenes
