#pragma once

#include "base_scene.hpp"

namespace cortex::mk3::ui::scenes {

class HelpScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Help"; }

    void draw(inkcell::Surface& surface) const override {
        using namespace inkcell;
        if (layout::render_min_size_notice(surface)) return;
        drawCommon(surface);
        Rect b = bodyRect(surface);
        layout::flat_panel(surface, b, theme::panel_bg());
        layout::section_rule(surface, {b.x + 2, b.y + 1}, b.w - 4, "keybindings");
        std::vector<std::string> rows = {
            "1  AgentTimeline  — protocol-aware live transcript",
            "2  Dashboard      — run health, counts, recent events",
            "3  Inspector      — bridge event log and raw tail",
            "?  Help           — current key map",
            "",
            "q / Ctrl-C        — quit cleanly",
            "r                 — toggle raw stream in timeline",
            "e                 — toggle protocol event rows",
            "Up/Down           — scroll timeline",
            "Ctrl-P/Ctrl-N     — page timeline",
            "",
            "Spec: no idle motion; loading/empty/error/populated states are first-class.",
        };
        int y = b.y + 3;
        for (const auto& row : rows) {
            surface.text({b.x + 2, y++}, text::truncate(row, b.w - 4), row.empty() ? theme::dim() : theme::text());
        }
        views::footer(surface, *model_, "all keybindings are discoverable here and in the footer");
    }
};

}  // namespace cortex::mk3::ui::scenes
