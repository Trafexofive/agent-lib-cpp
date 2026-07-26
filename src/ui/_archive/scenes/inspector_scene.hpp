#pragma once

#include "inkcell/widgets/scroll_view.hpp"
#include "base_scene.hpp"

namespace cortex::mk3::ui::scenes {

class InspectorScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Inspector"; }

    void draw(inkcell::Surface& surface) const override {
        using namespace inkcell;
        if (layout::render_min_size_notice(surface)) return;
        drawCommon(surface);
        Rect b = bodyRect(surface);
        Rect left{b.x, b.y, (b.w - 2) / 2, b.h};
        Rect right{left.right() + 2, b.y, b.right() - left.right() - 2, b.h};
        layout::flat_panel(surface, left, theme::panel_bg());
        layout::flat_panel(surface, right, theme::panel_bg());
        layout::section_rule(surface, {left.x + 2, left.y + 1}, left.w - 4, "bridge events");
        widgets::ScrollView().state(&model_->inspectorView).bordered(false).draw(
            surface, {left.x + 2, left.y + 3, left.w - 4, left.h - 4});
        layout::section_rule(surface, {right.x + 2, right.y + 1}, right.w - 4, "raw tail");
        Rect rawRect{right.x + 2, right.y + 3, right.w - 4, right.h - 4};
        layout::flat_panel(surface, rawRect, theme::panel_2());
        std::string tail = model_->raw.size() > 3000 ? model_->raw.substr(model_->raw.size() - 3000) : model_->raw;
        auto lines = splitDisplayLines(tail.empty() ? "raw stream empty" : tail);
        int start = std::max(0, static_cast<int>(lines.size()) - rawRect.h);
        for (int i = 0; i < rawRect.h && start + i < static_cast<int>(lines.size()); ++i)
            surface.text({rawRect.x + 1, rawRect.y + i},
                         text::truncate(lines[static_cast<size_t>(start + i)], rawRect.w - 2), theme::dim());
        views::footer(surface, *model_, "1 Agent · 2 Dashboard · r raw · t thoughts · q quit");
    }
};

}  // namespace cortex::mk3::ui::scenes
