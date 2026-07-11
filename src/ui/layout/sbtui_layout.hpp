#pragma once
// Generic drawing/layout helpers. No MK3 domain state.

#include <algorithm>
#include <string>

#include "inkcell/draw.hpp"
#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::layout {

inline void fill(inkcell::Surface& s, inkcell::Rect r, inkcell::Style st) {
    if (!r.empty()) s.fill(r, " ", st);
}

inline inkcell::Rect page(inkcell::Surface& surface) {
    return surface.bounds().inset(2);
}

inline void section_rule(inkcell::Surface& s, inkcell::Point p, int w, const std::string& label) {
    std::string text = "--- " + label + " ";
    if (static_cast<int>(text.size()) < w) text += std::string(static_cast<size_t>(w - text.size()), '-');
    s.text(p, inkcell::text::truncate(text, w), theme::dim());
}

inline void chip(inkcell::Surface& s, inkcell::Point p, const std::string& value, inkcell::Style st) {
    s.text(p, "[" + value + "]", st);
}

inline bool render_min_size_notice(inkcell::Surface& surface, int min_w = 80, int min_h = 18) {
    auto b = surface.bounds();
    if (b.w >= min_w && b.h >= min_h) return false;
    surface.clear(theme::base_bg());
    std::string msg = "resize terminal: minimum " + std::to_string(min_w) + "x" + std::to_string(min_h);
    surface.text({std::max(0, (b.w - static_cast<int>(msg.size())) / 2), std::max(0, b.h / 2)}, msg, theme::amber());
    return true;
}

inline void flat_panel(inkcell::Surface& surface, inkcell::Rect r, inkcell::Style st = theme::panel_bg()) {
    fill(surface, r, st);
}

inline void selected_row(inkcell::Surface& surface, inkcell::Rect r, const std::string& text, bool focused) {
    fill(surface, r, focused ? theme::panel_3() : theme::panel_2());
    surface.text({r.x + 1, r.y}, inkcell::text::truncate(std::string(focused ? "> " : "  ") + text, r.w - 2),
                 focused ? theme::selected_style() : theme::dim());
}

}  // namespace cortex::mk3::ui::layout
