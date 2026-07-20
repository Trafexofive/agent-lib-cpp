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

// ── Chat footer chrome (elevated status + prompt bar) ─────────────────
// Sits slightly above base_bg so the composer reads as a distinct surface
// without a heavy box border.
inline inkcell::Style footer_bg() {
    return inkcell::Style::normal().with_bg(
        color(inkcell::Color::rgb(24, 24, 24), inkcell::Color::rgb(10, 14, 22)));
}
inline inkcell::Style footer_bg_focus() {
    return inkcell::Style::normal().with_bg(
        color(inkcell::Color::rgb(30, 30, 30), inkcell::Color::rgb(14, 20, 32)));
}
inline inkcell::Style footer_dim() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(118, 118, 118), inkcell::Color::rgb(108, 120, 144));
    s.dim = true;
    return s;
}
inline inkcell::Style footer_text() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(200, 200, 200), inkcell::Color::rgb(210, 218, 232));
    return s;
}
inline inkcell::Style footer_bright() {
    auto s = footer_bg_focus();
    s.fg = color(inkcell::Color::rgb(242, 242, 242), inkcell::Color::rgb(245, 250, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_live() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(143, 181, 151), inkcell::Color::rgb(101, 227, 154));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_warn() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(201, 113, 113), inkcell::Color::rgb(255, 107, 122));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_accent_idle() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(70, 70, 70), inkcell::Color::rgb(40, 52, 72));
    return s;
}
inline inkcell::Style footer_accent_focus() {
    auto s = footer_bg_focus();
    s.fg = color(inkcell::Color::rgb(155, 172, 180), inkcell::Color::rgb(90, 220, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_accent_live() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(143, 181, 151), inkcell::Color::rgb(101, 227, 154));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_chip() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(160, 160, 160), inkcell::Color::rgb(150, 162, 186));
    return s;
}

}  // namespace cortex::mk3::ui::theme
