#pragma once
// Cortex-local theme tokens. Default is restrained Graphite; Neon remains
// available as an explicit user choice.

#include "inkcell/style.hpp"

namespace cortex::mk3::ui::theme {

enum class Variant { Graphite, Neon };
inline Variant activeVariant = Variant::Graphite;

inline const char* name() { return activeVariant == Variant::Graphite ? "graphite" : "neon"; }
inline void set(Variant variant) { activeVariant = variant; }
inline void toggle() {
    activeVariant = activeVariant == Variant::Graphite ? Variant::Neon : Variant::Graphite;
}

inline inkcell::Color color(inkcell::Color graphite, inkcell::Color neon) {
    return activeVariant == Variant::Graphite ? graphite : neon;
}

inline inkcell::Style base_bg() {
    return inkcell::Style::normal().with_bg(color(inkcell::Color::rgb(17, 17, 17), inkcell::Color::rgb(5, 7, 12)));
}
inline inkcell::Style panel_bg() {
    return inkcell::Style::normal().with_bg(color(inkcell::Color::rgb(21, 21, 21), inkcell::Color::rgb(8, 11, 18)));
}
inline inkcell::Style panel_2() {
    return inkcell::Style::normal().with_bg(color(inkcell::Color::rgb(27, 27, 27), inkcell::Color::rgb(13, 19, 32)));
}
inline inkcell::Style panel_3() {
    return inkcell::Style::normal().with_bg(color(inkcell::Color::rgb(39, 39, 39), inkcell::Color::rgb(18, 32, 52)));
}

inline inkcell::Style dim() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(125, 125, 125), inkcell::Color::rgb(116, 128, 152));
    s.dim = true;
    return s;
}
inline inkcell::Style text() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(205, 205, 205), inkcell::Color::rgb(215, 222, 234));
    return s;
}
inline inkcell::Style bright() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(238, 238, 238), inkcell::Color::rgb(239, 246, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style cyan() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(155, 172, 180), inkcell::Color::rgb(90, 220, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style green() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(143, 181, 151), inkcell::Color::rgb(101, 227, 154));
    s.bold = true;
    return s;
}
inline inkcell::Style amber() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(190, 164, 112), inkcell::Color::rgb(245, 185, 80));
    s.bold = true;
    return s;
}
inline inkcell::Style red() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(201, 113, 113), inkcell::Color::rgb(255, 107, 122));
    s.bold = true;
    return s;
}
inline inkcell::Style selected_style() {
    auto s = panel_3();
    s.fg = color(inkcell::Color::rgb(231, 231, 231), inkcell::Color::rgb(101, 227, 154));
    s.bold = true;
    return s;
}

}  // namespace cortex::mk3::ui::theme
