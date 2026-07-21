#pragma once
// Cortex-local theme tokens.
// Graphite default is NOT monotone — muted base + cyan/amber/green/violet accents,
// bold / dim / italic hierarchy. Neon is the high-chroma alternate.

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

// ── Surfaces ─────────────────────────────────────────────────────────
inline inkcell::Style base_bg() {
    return inkcell::Style::normal().with_bg(
        color(inkcell::Color::rgb(14, 14, 16), inkcell::Color::rgb(5, 7, 12)));
}
inline inkcell::Style panel_bg() {
    return inkcell::Style::normal().with_bg(
        color(inkcell::Color::rgb(20, 20, 23), inkcell::Color::rgb(8, 11, 18)));
}
inline inkcell::Style panel_2() {
    return inkcell::Style::normal().with_bg(
        color(inkcell::Color::rgb(26, 26, 30), inkcell::Color::rgb(13, 19, 32)));
}
inline inkcell::Style panel_3() {
    return inkcell::Style::normal().with_bg(
        color(inkcell::Color::rgb(36, 36, 42), inkcell::Color::rgb(18, 32, 52)));
}

// ── Type ramp ────────────────────────────────────────────────────────
inline inkcell::Style dim() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(110, 110, 118), inkcell::Color::rgb(100, 112, 136));
    s.dim = true;
    return s;
}
inline inkcell::Style muted() {
    // dim-ish without SGR dim — readable secondary labels
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(140, 140, 150), inkcell::Color::rgb(130, 142, 168));
    return s;
}
inline inkcell::Style text() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(210, 210, 216), inkcell::Color::rgb(215, 222, 234));
    return s;
}
inline inkcell::Style bright() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(244, 244, 248), inkcell::Color::rgb(239, 246, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style italic() {
    auto s = text();
    s.italic = true;
    return s;
}
inline inkcell::Style italic_dim() {
    auto s = dim();
    s.italic = true;
    return s;
}
inline inkcell::Style italic_accent() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(155, 172, 180), inkcell::Color::rgb(90, 220, 255));
    s.italic = true;
    return s;
}

// ── Accents (use deliberately — not every label) ─────────────────────
inline inkcell::Style cyan() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(120, 175, 190), inkcell::Color::rgb(90, 220, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style cyan_soft() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(100, 145, 158), inkcell::Color::rgb(70, 180, 210));
    return s;
}
inline inkcell::Style green() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(130, 185, 145), inkcell::Color::rgb(101, 227, 154));
    s.bold = true;
    return s;
}
inline inkcell::Style green_soft() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(110, 155, 125), inkcell::Color::rgb(80, 190, 130));
    return s;
}
inline inkcell::Style amber() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(200, 165, 100), inkcell::Color::rgb(245, 185, 80));
    s.bold = true;
    return s;
}
inline inkcell::Style amber_soft() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(170, 140, 90), inkcell::Color::rgb(210, 160, 70));
    return s;
}
inline inkcell::Style red() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(210, 120, 120), inkcell::Color::rgb(255, 107, 122));
    s.bold = true;
    return s;
}
inline inkcell::Style violet() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(165, 145, 200), inkcell::Color::rgb(180, 150, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style violet_soft() {
    auto s = panel_bg();
    s.fg = color(inkcell::Color::rgb(140, 125, 170), inkcell::Color::rgb(150, 130, 220));
    return s;
}
inline inkcell::Style selected_style() {
    auto s = panel_3();
    s.fg = color(inkcell::Color::rgb(240, 240, 245), inkcell::Color::rgb(101, 227, 154));
    s.bold = true;
    return s;
}

// Kind → accent (list chips / headers)
inline inkcell::Style kindAccent(const std::string& kind, bool bold = true) {
    inkcell::Style s = panel_bg();
    if (kind == "agent")
        s.fg = color(inkcell::Color::rgb(120, 175, 190), inkcell::Color::rgb(90, 220, 255));
    else if (kind == "tool")
        s.fg = color(inkcell::Color::rgb(130, 185, 145), inkcell::Color::rgb(101, 227, 154));
    else if (kind == "feed")
        s.fg = color(inkcell::Color::rgb(200, 165, 100), inkcell::Color::rgb(245, 185, 80));
    else if (kind == "workflow")
        s.fg = color(inkcell::Color::rgb(165, 145, 200), inkcell::Color::rgb(180, 150, 255));
    else if (kind == "harness")
        s.fg = color(inkcell::Color::rgb(190, 150, 120), inkcell::Color::rgb(230, 170, 120));
    else if (kind == "skill" || kind == "prompt")
        s.fg = color(inkcell::Color::rgb(150, 160, 190), inkcell::Color::rgb(160, 180, 230));
    else
        s.fg = color(inkcell::Color::rgb(150, 150, 158), inkcell::Color::rgb(140, 150, 170));
    s.bold = bold;
    return s;
}

// ── Chat footer chrome ───────────────────────────────────────────────
inline inkcell::Style footer_bg() {
    return inkcell::Style::normal().with_bg(
        color(inkcell::Color::rgb(22, 22, 26), inkcell::Color::rgb(10, 14, 22)));
}
inline inkcell::Style footer_bg_focus() {
    return inkcell::Style::normal().with_bg(
        color(inkcell::Color::rgb(28, 28, 34), inkcell::Color::rgb(14, 20, 32)));
}
inline inkcell::Style footer_dim() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(110, 110, 120), inkcell::Color::rgb(108, 120, 144));
    s.dim = true;
    return s;
}
inline inkcell::Style footer_text() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(200, 200, 208), inkcell::Color::rgb(210, 218, 232));
    return s;
}
inline inkcell::Style footer_bright() {
    auto s = footer_bg_focus();
    s.fg = color(inkcell::Color::rgb(245, 245, 250), inkcell::Color::rgb(245, 250, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_live() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(130, 185, 145), inkcell::Color::rgb(101, 227, 154));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_warn() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(210, 120, 120), inkcell::Color::rgb(255, 107, 122));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_accent_idle() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(70, 70, 78), inkcell::Color::rgb(40, 52, 72));
    return s;
}
inline inkcell::Style footer_accent_focus() {
    auto s = footer_bg_focus();
    s.fg = color(inkcell::Color::rgb(120, 175, 190), inkcell::Color::rgb(90, 220, 255));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_accent_live() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(130, 185, 145), inkcell::Color::rgb(101, 227, 154));
    s.bold = true;
    return s;
}
inline inkcell::Style footer_chip() {
    auto s = footer_bg();
    s.fg = color(inkcell::Color::rgb(155, 155, 165), inkcell::Color::rgb(150, 162, 186));
    return s;
}

}  // namespace cortex::mk3::ui::theme
