#pragma once
// Agent / History — the only working surface besides Welcome.
// Block focus + nested sub-agent history drill-down.

#include "inkcell/widgets/scroll_view.hpp"
#include "inkcell/widgets/textarea.hpp"
#include "base_scene.hpp"

namespace cortex::mk3::ui::scenes {

class AgentScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Agent"; }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;

        // Timeline / history navigation owns keys when focused.
        if (model_->timelineFocus || !model_->atRoot() || !model_->composer.focused) {
            if (event.code == KeyCode::Escape) {
                if (model_->goBack()) return true;
                model_->focusComposer();
                return true;
            }
            if (event.code == KeyCode::ArrowUp || (event.code == KeyCode::Character && event.ch == 'k')) {
                model_->selectDelta(-1);
                return true;
            }
            if (event.code == KeyCode::ArrowDown || (event.code == KeyCode::Character && event.ch == 'j')) {
                model_->selectDelta(1);
                return true;
            }
            if (event.code == KeyCode::Enter) {
                if (model_->enterSelected()) return true;
                return true;  // consume even if not drillable
            }
            if (event.code == KeyCode::Backspace ||
                (event.code == KeyCode::Character && (event.ch == 'h' || event.ch == 'H'))) {
                if (model_->goBack()) return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'i' && model_->atRoot()) {
                model_->focusComposer();
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'g') {
                // refresh nested snapshot
                model_->refreshNested();
                return true;
            }
            // Page-ish scroll of the transcript viewport while selecting.
            if (event.code == KeyCode::Character && event.ch == 'u') {
                model_->transcriptView.scroll_by(-model_->transcriptView.viewport_h / 2);
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'd') {
                model_->transcriptView.scroll_by(model_->transcriptView.viewport_h / 2);
                return true;
            }
            return false;
        }

        // Composer owns focus.
        if (event.code == KeyCode::Escape) {
            model_->focusTimeline();
            return true;
        }
        if (event.code == KeyCode::Enter) {
            model_->submitComposer();
            return true;
        }
        if (model_->composer.handle_key(event)) return true;
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        using namespace inkcell;
        if (layout::render_min_size_notice(surface)) return;
        surface.clear(theme::base_bg());
        Rect p = layout::page(surface);

        // Topbar — flat, no box spam
        surface.text({p.x, p.y}, "CORTEX MK3", theme::cyan());
        std::string path = model_->breadcrumb();
        surface.text({p.x, p.y + 1}, text::truncate(path, p.w), theme::dim());

        int chipY = p.y + 3;
        layout::chip(surface, {p.x, chipY},
                     model_->running ? "● live" : model_->done ? "✓ done" : "○ idle",
                     model_->failed ? theme::red() : model_->running ? theme::green() : theme::dim());
        layout::chip(surface, {p.x + 11, chipY},
                     nonempty(cfg_.provider, "?") + "/" + nonempty(cfg_.model, "default"), theme::dim());
        layout::chip(surface, {p.x + 40, chipY},
                     model_->timelineFocus ? "focus: history" : "focus: composer", theme::text());
        layout::section_rule(surface, {p.x, p.y + 5}, p.w, model_->atRoot() ? "history" : "sub-agent history");

        bool showComposer = model_->atRoot();
        int composerH = showComposer ? std::max(5, std::min(7, p.h / 5)) : 0;
        int bodyTop = p.y + 7;
        int bodyH = std::max(3, p.h - 8 - composerH);
        Rect tview{p.x, bodyTop, p.w, bodyH};

        layout::flat_panel(surface, tview, theme::panel_bg());
        widgets::ScrollView()
            .state(&model_->transcriptView)
            .bordered(false)
            .draw(surface, {tview.x + 1, tview.y + 1, std::max(1, tview.w - 2), std::max(1, tview.h - 2)});

        if (showComposer) {
            Rect composer{p.x, p.bottom() - composerH - 1, p.w, composerH};
            widgets::TextArea()
                .state(&model_->composer)
                .placeholder(model_->running ? "agent running…"
                                             : "message · Enter send · Esc history focus · j/k select · Enter drill")
                .focused(model_->composer.focused && !model_->timelineFocus)
                .title(model_->timelineFocus ? "composer" : "composer (focus)")
                .draw(surface, composer);
        }

        std::string hints;
        if (!model_->atRoot()) {
            hints = "Esc/Backspace back · j/k select · Enter drill · g refresh · path in header";
        } else if (model_->timelineFocus) {
            hints = "j/k select · Enter open sub-agent · i composer · Esc composer · q quit";
        } else {
            hints = "Enter send · Esc history · j/k after Esc · q quit";
        }
        std::string right = "pending " + std::to_string(model_->pendingOps) + " · " +
                            std::to_string(model_->actionCount) + " actions";
        surface.text({p.x, p.bottom() - 1}, text::truncate(hints, std::max(0, p.w - static_cast<int>(right.size()) - 2)),
                     theme::dim());
        surface.text({std::max(p.x, p.right() - static_cast<int>(right.size())), p.bottom() - 1}, right, theme::dim());
    }

   private:
    void handle(const inkcell::Action& action) override {
        if (action.is("shell.focus_composer")) model_->focusComposer();
        else if (action.is("shell.focus_timeline")) model_->focusTimeline();
        else if (action.is("shell.toggle_raw")) {
            model_->showRaw = !model_->showRaw;
            model_->rebuildViews();
        } else if (action.is("shell.toggle_thoughts")) {
            model_->showThoughts = !model_->showThoughts;
            model_->rebuildViews();
        } else if (action.is("scroll.up")) model_->selectDelta(-1);
        else if (action.is("scroll.down")) model_->selectDelta(1);
        else if (action.is("history.enter")) model_->enterSelected();
        else if (action.is("history.back")) model_->goBack();
    }
};

}  // namespace cortex::mk3::ui::scenes
