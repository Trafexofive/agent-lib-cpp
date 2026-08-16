#pragma once
// Textured floating pill dock — 3-row rounded capsule + sliding thumb + ▀ track.
// Restored from the denser design; 4-section IA + dock-focus ring kept.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

struct PillItem {
    std::string key;
    std::string label;
};

inline float easeInOut(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
}

inline inkcell::Color lerpColor(inkcell::Color a, inkcell::Color b, float t) {
    t = std::max(0.f, std::min(1.f, t));
    auto L = [&](uint8_t x, uint8_t y) -> uint8_t {
        return static_cast<uint8_t>(x + static_cast<int>((y - x) * t));
    };
    return inkcell::Color::rgb(L(a.r, b.r), L(a.g, b.g), L(a.b, b.b));
}

struct PillTheme {
    inkcell::Color shellBg;
    inkcell::Color border;
    inkcell::Color borderFocus;
    inkcell::Color idleFg;
    inkcell::Color keyFg;
    inkcell::Color activeFg;
    inkcell::Color activeBg;
    inkcell::Color thumb;
    inkcell::Color track;
    inkcell::Color shadow;
};

inline PillTheme pillTheme(bool dockFocused) {
    if (theme::activeVariant == theme::Variant::Neon) {
        return PillTheme{
            inkcell::Color::rgb(12, 16, 26),
            dockFocused ? inkcell::Color::rgb(90, 220, 255) : inkcell::Color::rgb(40, 56, 84),
            inkcell::Color::rgb(90, 220, 255),
            inkcell::Color::rgb(110, 124, 150),
            inkcell::Color::rgb(70, 180, 210),
            inkcell::Color::rgb(8, 14, 20),
            inkcell::Color::rgb(90, 220, 255),
            inkcell::Color::rgb(90, 220, 255),
            inkcell::Color::rgb(22, 30, 46),
            inkcell::Color::rgb(2, 4, 8),
        };
    }
    return PillTheme{
        inkcell::Color::rgb(30, 30, 34),
        dockFocused ? inkcell::Color::rgb(120, 175, 190) : inkcell::Color::rgb(62, 62, 70),
        inkcell::Color::rgb(120, 175, 190),
        inkcell::Color::rgb(125, 125, 135),
        inkcell::Color::rgb(100, 145, 158),
        inkcell::Color::rgb(18, 18, 20),
        inkcell::Color::rgb(210, 215, 220),
        inkcell::Color::rgb(160, 180, 188),
        inkcell::Color::rgb(40, 40, 46),
        inkcell::Color::rgb(8, 8, 10),
    };
}

// Returns height used (3). bottomY is the last row of the dock.
inline int drawPillDock(inkcell::Surface& s, int pageX, int pageW, int bottomY,
                        const std::vector<PillItem>& items, int current, int previous, float animT,
                        bool dockFocused) {
    if (items.empty() || pageW < 28) return 0;

    const PillTheme T = pillTheme(dockFocused);
    const int dockH = 3;

    // Labels always show key for texture/discoverability
    std::vector<std::string> labels;
    std::vector<int> widths;
    int inner = 0;
    for (const auto& it : items) {
        std::string lab = "  " + it.key + " " + it.label + "  ";
        labels.push_back(lab);
        int w = inkcell::text::display_width(lab);
        widths.push_back(w);
        inner += w;
    }

    int boxW = std::min(pageW - 2, inner + 4);
    if (boxW < 24) boxW = std::min(pageW - 2, 24);
    int boxX = pageX + std::max(0, (pageW - boxW) / 2);
    int boxY = bottomY - (dockH - 1);
    inkcell::Rect box{boxX, boxY, boxW, dockH};

    auto shell = inkcell::Style::normal().with_bg(T.shellBg).with_fg(T.border);
    s.fill(box, " ", shell);
    s.box(box, inkcell::BorderStyle::Rounded, shell.with_fg(T.border));

    // Focus glow on top edge
    if (dockFocused) {
        for (int x = box.x + 2; x < box.right() - 2; ++x)
            s.put({x, box.y}, "─", shell.with_fg(T.borderFocus));
    }

    int cx0 = box.x + 1;
    int cyLabel = box.y + 1;
    int contentW = box.w - 2;

    struct SegGeom {
        int x0, w;
    };
    std::vector<SegGeom> geom;
    int used = 0;
    for (int w : widths) used += w;
    int cursor = cx0;
    if (used < contentW) cursor = cx0 + (contentW - used) / 2;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        geom.push_back({cursor, widths[static_cast<size_t>(i)]});
        cursor += widths[static_cast<size_t>(i)];
    }

    float e = easeInOut(animT);
    int ti = std::max(0, std::min(current, static_cast<int>(geom.size()) - 1));
    int thumbX = geom[static_cast<size_t>(ti)].x0;
    int thumbW = geom[static_cast<size_t>(ti)].w;
    if (previous >= 0 && previous < static_cast<int>(geom.size()) && animT < 1.f) {
        int px = geom[static_cast<size_t>(previous)].x0;
        int pw = geom[static_cast<size_t>(previous)].w;
        thumbX = static_cast<int>(std::lround(px + (thumbX - px) * e));
        thumbW = static_cast<int>(std::lround(pw + (thumbW - pw) * e));
    }
    if (thumbX < cx0) thumbX = cx0;
    if (thumbX + thumbW > cx0 + contentW) thumbW = std::max(1, cx0 + contentW - thumbX);

    auto thumbSt = inkcell::Style::normal().with_bg(T.activeBg).with_fg(T.activeFg);
    thumbSt.bold = true;
    s.fill({thumbX, cyLabel, thumbW, 1}, " ", thumbSt);

    // Bottom track glow under thumb (texture)
    std::string bar;
    for (int i = 0; i < thumbW; ++i) bar += "▀";
    auto trackSt = inkcell::Style::normal().with_bg(T.shellBg).with_fg(T.thumb);
    s.text({thumbX, box.y + 2}, bar, trackSt);
    // restore corners
    s.text({box.x, box.y + 2}, "╰", shell.with_fg(T.border));
    s.text({box.right() - 1, box.y + 2}, "╯", shell.with_fg(T.border));
    // side borders on label row
    s.text({box.x, cyLabel}, "│", shell.with_fg(T.border));
    s.text({box.right() - 1, cyLabel}, "│", shell.with_fg(T.border));
    // top corners
    s.text({box.x, box.y}, "╭", shell.with_fg(dockFocused ? T.borderFocus : T.border));
    s.text({box.right() - 1, box.y}, "╮", shell.with_fg(dockFocused ? T.borderFocus : T.border));

    // Segment labels — key dim/cyan, label bold when active
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& it = items[static_cast<size_t>(i)];
        const auto& g = geom[static_cast<size_t>(i)];
        bool on = (g.x0 + g.w / 2) >= thumbX && (g.x0 + g.w / 2) < thumbX + thumbW;

        // Draw as: "  " + key + " " + label + "  "
        int x = g.x0;
        auto bg = on ? T.activeBg : T.shellBg;
        auto pad = inkcell::Style::normal().with_bg(bg).with_fg(on ? T.activeFg : T.idleFg);
        s.text({x, cyLabel}, "  ", pad);
        x += 2;

        auto keySt = inkcell::Style::normal().with_bg(bg);
        if (on) {
            keySt.fg = T.activeFg;
            keySt.bold = true;
        } else {
            keySt.fg = T.keyFg;
            keySt.italic = true;  // shortcut keys italic — texture
        }
        s.text({x, cyLabel}, it.key, keySt);
        x += inkcell::text::display_width(it.key);

        s.text({x, cyLabel}, " ", pad);
        x += 1;

        auto labSt = inkcell::Style::normal().with_bg(bg);
        if (on) {
            labSt.fg = T.activeFg;
            labSt.bold = true;
        } else if (i == current) {
            labSt.fg = lerpColor(T.idleFg, T.activeBg, 0.55f);
            labSt.bold = true;
        } else {
            labSt.fg = T.idleFg;
            labSt.dim = true;
        }
        s.text({x, cyLabel}, it.label, labSt);
    }

    return dockH;
}

}  // namespace cortex::mk3::ui::components
