#pragma once
// Cortex-local theme tokens. No state, no rendering decisions.

#include "inkcell/style.hpp"

namespace cortex::mk3::ui::theme {

inline inkcell::Style base_bg() { return inkcell::Style::normal().with_bg(inkcell::Color::rgb(5, 7, 12)); }
inline inkcell::Style panel_bg() { return inkcell::Style::normal().with_bg(inkcell::Color::rgb(8, 11, 18)); }
inline inkcell::Style panel_2() { return inkcell::Style::normal().with_bg(inkcell::Color::rgb(13, 19, 32)); }
inline inkcell::Style panel_3() { return inkcell::Style::normal().with_bg(inkcell::Color::rgb(18, 32, 52)); }

inline inkcell::Style dim() {
    auto s = panel_bg();
    s.fg = inkcell::Color::rgb(116, 128, 152);
    s.dim = true;
    return s;
}

inline inkcell::Style text() {
    auto s = panel_bg();
    s.fg = inkcell::Color::rgb(215, 222, 234);
    return s;
}

inline inkcell::Style bright() {
    auto s = panel_bg();
    s.fg = inkcell::Color::rgb(239, 246, 255);
    s.bold = true;
    return s;
}

inline inkcell::Style cyan() {
    auto s = panel_bg();
    s.fg = inkcell::Color::rgb(90, 220, 255);
    s.bold = true;
    return s;
}

inline inkcell::Style green() {
    auto s = panel_bg();
    s.fg = inkcell::Color::rgb(101, 227, 154);
    s.bold = true;
    return s;
}

inline inkcell::Style amber() {
    auto s = panel_bg();
    s.fg = inkcell::Color::rgb(245, 185, 80);
    s.bold = true;
    return s;
}

inline inkcell::Style red() {
    auto s = panel_bg();
    s.fg = inkcell::Color::rgb(255, 107, 122);
    s.bold = true;
    return s;
}

inline inkcell::Style selected_style() {
    auto s = panel_3();
    s.fg = inkcell::Color::rgb(101, 227, 154);
    s.bold = true;
    return s;
}

}  // namespace cortex::mk3::ui::theme
