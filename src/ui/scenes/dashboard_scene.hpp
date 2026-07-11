#pragma once

#include "base_scene.hpp"

namespace cortex::mk3::ui::scenes {

class DashboardScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Dashboard"; }

    void draw(inkcell::Surface& surface) const override {
        using namespace inkcell;
        if (layout::render_min_size_notice(surface)) return;
        drawCommon(surface);
        Rect b = bodyRect(surface);
        int navW = b.w >= 100 ? 22 : 0;
        if (navW) views::nav(surface, {b.x, b.y, navW, b.h}, name());
        Rect main{b.x + (navW ? navW + 2 : 0), b.y, b.w - (navW ? navW + 2 : 0), b.h};
        layout::flat_panel(surface, main, theme::panel_bg());
        layout::section_rule(surface, {main.x + 2, main.y + 1}, main.w - 4, "run health");
        int y = main.y + 3;
        views::metric(surface, {main.x + 2, y, 24, 4}, "stream", std::to_string(model_->tokenBytes) + " bytes",
                      theme::cyan());
        views::metric(surface, {main.x + 28, y, 18, 4}, "actions", std::to_string(model_->actionCount), theme::amber());
        views::metric(surface, {main.x + 48, y, 18, 4}, "results", std::to_string(model_->resultCount), theme::green());
        layout::section_rule(surface, {main.x + 2, y + 6}, main.w - 4, "recent protocol events");
        Rect list{main.x + 2, y + 8, main.w - 4, main.h - 10};
        layout::flat_panel(surface, list, theme::panel_2());
        if (model_->eventLog.empty()) {
            views::state_block(surface, list, PageState::Empty, "events", *model_);
        } else {
            int row = 0;
            for (int i = static_cast<int>(model_->eventLog.size()) - 1; i >= 0 && row < list.h - 1; --i, ++row) {
                surface.text({list.x + 1, list.y + row},
                             text::truncate("│ " + model_->eventLog[static_cast<size_t>(i)], list.w - 2), theme::text());
            }
        }
        views::footer(surface, *model_, "1 Agent · 3 Inspector · ? Help · q quit");
    }
};

}  // namespace cortex::mk3::ui::scenes
