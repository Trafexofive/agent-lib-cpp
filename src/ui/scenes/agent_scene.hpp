#pragma once

#include "inkcell/widgets/scroll_view.hpp"
#include "inkcell/widgets/textarea.hpp"
#include "base_scene.hpp"

namespace cortex::mk3::ui::scenes {

class AgentScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Agent"; }

    bool on_key(const inkcell::KeyEvent& event) override {
        // When composer owns focus, typing goes to TextArea. Enter submits.
        // Esc releases focus to timeline scroll. Ctrl-C handled by engine.
        if (!model_->composer.focused) return false;

        if (event.code == inkcell::KeyCode::Escape) {
            model_->composer.focused = false;
            return true;
        }
        if (event.code == inkcell::KeyCode::Enter) {
            model_->submitComposer();
            return true;
        }
        // Allow arrow-up/down to still scroll timeline only when empty composer? keep in textarea.
        if (model_->composer.handle_key(event)) return true;
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        using namespace inkcell;
        if (layout::render_min_size_notice(surface)) return;
        drawCommon(surface);

        Rect b = bodyRect(surface);
        int navW = b.w >= 100 ? 22 : 0;
        if (navW) views::nav(surface, {b.x, b.y, navW, b.h}, name());

        Rect main{b.x + (navW ? navW + 2 : 0), b.y, b.w - (navW ? navW + 2 : 0), b.h};
        layout::flat_panel(surface, main, theme::panel_bg());

        int composerH = std::max(5, std::min(8, main.h / 4));
        Rect timeline{main.x + 2, main.y + 1, main.w - 4, main.h - composerH - 3};
        Rect composer{main.x + 2, main.bottom() - composerH - 1, main.w - 4, composerH};

        layout::section_rule(surface, {timeline.x, timeline.y}, timeline.w, "timeline");
        Rect tview{timeline.x, timeline.y + 2, timeline.w, std::max(1, timeline.h - 2)};
        if (model_->timelineState == PageState::Error && model_->rows.empty()) {
            views::state_block(surface, tview, PageState::Error, "agent stream", *model_);
        } else {
            widgets::ScrollView().state(&model_->transcriptView).bordered(false).draw(surface, tview);
        }

        // Composer: background panel + TextArea without double-box spam if possible.
        // TextArea draws its own panel; use it as the single containment for input.
        widgets::TextArea()
            .state(&model_->composer)
            .placeholder(model_->running ? "agent running…" : "message · Enter send · Esc timeline · i composer")
            .focused(model_->composer.focused)
            .title(model_->composer.focused ? "composer (focus)" : "composer")
            .draw(surface, composer);

        views::footer(surface, *model_,
                      model_->composer.focused
                          ? "Enter send · Esc timeline · 1/2/3/? routes when unfocused · q quit"
                          : "i composer · ↑↓ scroll · 1/2/3/? routes · t thoughts · r raw · q quit");
    }

   private:
    void handle(const inkcell::Action& action) override {
        if (model_->composer.focused) {
            // Global single-letter binds are already blocked by on_key for typing.
            // Still ignore scroll while typing.
            if (action.is("scroll.up") || action.is("scroll.down") || action.is("scroll.page_up") ||
                action.is("scroll.page_down"))
                return;
        }
        if (action.is("scroll.up")) model_->transcriptView.scroll_by(-2);
        else if (action.is("scroll.down")) model_->transcriptView.scroll_by(2);
        else if (action.is("scroll.page_up")) model_->transcriptView.scroll_by(-model_->transcriptView.viewport_h);
        else if (action.is("scroll.page_down")) model_->transcriptView.scroll_by(model_->transcriptView.viewport_h);
        else if (action.is("scroll.end")) model_->transcriptView.scroll_to_end();
    }
};

}  // namespace cortex::mk3::ui::scenes
